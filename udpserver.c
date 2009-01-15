/*
 * Project: udptunnel
 * File: udpserver.c
 *
 * Copyright (C) 2009 Daniel Meekins
 * Contact: dmeekins - gmail
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/select.h>
#include "common.h"
#include "list.h"
#include "client.h"
#include "message.h"
#include "socket.h"

static int running = 1;
static int next_client_id = 1;
static int ipver = SOCK_IPV4;

/* internal functions */
int handle_message(uint16_t id, uint8_t msg_type, char *data, int data_len,
                   socket_t *from, list_t *clients, fd_set *client_fds);
void disconnect_and_remove_client(uint16_t id, list_t *clients, fd_set *fds);
void usage(char *prog);
void signal_handler(int sig);

/*
 * UDP Tunnel server main(). Handles program arguments, initializes everything,
 * and runs the main loop.
 */
int main(int argc, char *argv[])
{
    char host_str[ADDRSTRLEN];
    char port_str[ADDRSTRLEN];
    
    list_t *clients = NULL;
    socket_t *udp_sock = NULL;
    socket_t *udp_from = NULL;
    char data[MSG_MAX_LEN];

    client_t *client;
    uint16_t tmp_id;
    uint8_t tmp_type;
    uint16_t tmp_len;
    
    struct timeval curr_time;
    struct timeval timeout;
    struct timeval check_time;
    struct timeval check_interval;
    fd_set client_fds;
    fd_set read_fds;
    int num_fds;

    int i;
    int ret;
    
    signal(SIGINT, &signal_handler);

    while((ret = getopt(argc, argv, "6")) != -1)
    {
        switch(ret)
        {
            case '6':
                ipver = SOCK_IPV6;
                break;

            default:
                usage(argv[0]);
                exit(1);
        }
    }

    /* Get the port and address to listen on from command line */
    if(argc - optind == 1)
    {
        strncpy(port_str, argv[optind], sizeof(port_str));
        port_str[sizeof(port_str)-1] = 0;
        host_str[0] = 0;
    }
    else if(argc - optind == 2)
    {
        strncpy(host_str, argv[optind], sizeof(host_str));
        strncpy(port_str, argv[optind+1], sizeof(port_str));
        host_str[sizeof(host_str)-1] = 0;
        port_str[sizeof(port_str)-1] = 0;
    }
    else
    {
        usage(argv[0]);
        exit(1);
    }

    /* Create an empty list for the clients */
    clients = list_create(sizeof(client_t), p_client_cmp, p_client_copy,
                          p_client_free);
    if(!clients)
        goto done;

    /* Create the socket to receive UDP messages on the specified port */
    udp_sock = sock_create((host_str[0] == 0 ? NULL : host_str), port_str,
                           ipver, SOCK_TYPE_UDP, 1, 1);
    if(!udp_sock)
        goto done;

    /* Create empty udp socket for getting source address of udp packets */
    udp_from = sock_create(NULL, NULL, ipver, SOCK_TYPE_UDP, 0, 0);
    if(!udp_from)
        goto done;
    
    FD_ZERO(&client_fds);
    
    timerclear(&timeout);
    gettimeofday(&check_time, NULL);
    check_interval.tv_sec = 0;
    check_interval.tv_usec = 500000;
    
    while(running)
    {
        if(!timerisset(&timeout))
            timeout.tv_usec = 50000;

        /* Reset the file desc. set */
        read_fds = client_fds;
        FD_SET(SOCK_FD(udp_sock), &read_fds);

        ret = select(FD_SETSIZE, &read_fds, NULL, NULL, &timeout);
        PERROR_GOTO(ret < 0, "select", done);
        num_fds = ret;

        gettimeofday(&curr_time, NULL);

        /* Go through all the clients and check if didn't get an ACK for sent
           data during the timeout period */
        if(timercmp(&curr_time, &check_time, >))
        {
            for(i = 0; i < LIST_LEN(clients); i++)
            {
                client = list_get_at(clients, i);
                ret = client_check_and_resend(client);

                if(ret == -2)
                {
                    disconnect_and_remove_client(CLIENT_ID(client),
                                                 clients, &client_fds);
                    i--;
                }
            }

            /* Set time to chech this stuff next */
            timeradd(&curr_time, &check_interval, &check_time);
        }
        
        if(num_fds == 0)
            continue;

        /* Get any data received on the UDP socket */
        if(FD_ISSET(SOCK_FD(udp_sock), &read_fds))
        {
            ret = msg_recv_msg(udp_sock, udp_from, data, sizeof(data),
                               &tmp_id, &tmp_type, &tmp_len);
            
            if(ret == 0)
                ret = handle_message(tmp_id, tmp_type, data, tmp_len,
                                     udp_from, clients, &client_fds);            
            if(ret == -2)
                disconnect_and_remove_client(tmp_id, clients, &client_fds);

            num_fds--;
        }

        /* Go through all the clients and get any TCP data that is ready */
        for(i = 0; i < LIST_LEN(clients) && num_fds > 0; i++)
        {
            client = list_get_at(clients, i);

            if(client_tcp_fd_isset(client, &read_fds))
            {
                ret = client_recv_tcp_data(client);
                if(ret == 0)
                    ret = client_send_udp_data(client);
                else if(ret == 1)
                    usleep(1000); /* Quick hack so doesn't use 100% CPU if
                                     data wasn't ready yet (waiting for ack) */
                if(ret == -2)
                {
                    disconnect_and_remove_client(CLIENT_ID(client),
                                                 clients, &client_fds);
                    i--; /* Since there will be one less element in list */
                }

                num_fds--;
            }
        }
    }
    
  done:
    if(DEBUG)
        printf("Cleaning up...\n");
    if(clients)
        list_free(clients);
    if(udp_sock)
    {
        sock_close(udp_sock);
        sock_free(udp_sock);
    }
    if(udp_from)
        sock_free(udp_from);
    if(DEBUG)
        printf("Goodbye.\n");
    
    return 0;
}

/*
 * Closes the client's TCP socket (not UDP, since it is shared) and remove the
 * client from the fd set and client list.
 */
void disconnect_and_remove_client(uint16_t id, list_t *clients, fd_set *fds)
{
    client_t *c;

    if(id == 0)
        return;
    
    c = list_get(clients, &id);
    if(!c)
        return;

    client_remove_tcp_fd_from_set(c, fds);
    client_disconnect_tcp(c);
    list_delete(clients, &id);
}

/*
 * Handles the message received from the UDP tunnel. Returns 0 for success, -1
 * for some error that it handled, and -2 if the connection should be
 * disconnected.
 */
int handle_message(uint16_t id, uint8_t msg_type, char *data, int data_len,
                   socket_t *from, list_t *clients, fd_set *client_fds)
{
    client_t *c = NULL;
    client_t *c2 = NULL;
    socket_t *tcp_sock = NULL;
    int ret = 0;
    
    if(id != 0)
    {
        c = list_get(clients, &id);
        if(!c)
            return -1;
    }

    if(id == 0 && msg_type != MSG_TYPE_HELLO)
        return -2;
    
    switch(msg_type)
    {
        case MSG_TYPE_GOODBYE:
            ret = -2;
            break;
            
        /* Data in the hello message will be like "hostname port", possibly
           without the null terminator. This will look for the space and
           parse out the hostname or ip address and port number */
        case MSG_TYPE_HELLO:
        {
            int i;
            char port[6]; /* need this so port str can have null term. */

            if(id != 0)
                break;

            /* look for the space separating the host and port */
            for(i = 0; i < data_len; i++)
                if(data[i] == ' ')
                    break;
            if(i == data_len)
                break;

            /* null terminate the host and get the port number to the string */
            data[i++] = 0;
            strncpy(port, data+i, data_len-i);
            port[data_len-i] = 0;

            /* Create and unconnected TCP socket for the remote host, the
               client itself, add it to the list of clients */
            tcp_sock = sock_create(data, port, ipver, SOCK_TYPE_TCP, 0, 0);
            ERROR_GOTO(tcp_sock == NULL, "Error creating tcp socket", error);
            c = client_create(next_client_id, tcp_sock, from, 0);
            sock_free(tcp_sock);
            ERROR_GOTO(c == NULL, "Error creating client", error);
            next_client_id++;
            c2 = list_add(clients, c);
            ERROR_GOTO(c2 == NULL, "Error adding client to list", error);
            client_free(c);

            /* Send the Hello ACK message if created client successfully */
            client_send_helloack(c2);
            
            break;
        }

        /* Can connect to TCP connection once received the Hello ACK */
        case MSG_TYPE_HELLOACK:
            client_got_helloack(c);
            client_connect_tcp(c);
            client_add_tcp_fd_to_set(c, client_fds);
            break;

        case MSG_TYPE_KEEPALIVE:
            /* TODO */
            break;

        /* Receives the data it got from the UDP tunnel and sends it to the
           TCP connection. */
        case MSG_TYPE_DATA0:
        case MSG_TYPE_DATA1:
            ret = client_got_udp_data(c, data, data_len, msg_type);
            if(ret == 0)
                ret = client_send_tcp_data(c);
            break;

        /* Receives the ACK from the UDP tunnel to set the internal client
           state. */
        case MSG_TYPE_ACK0:
        case MSG_TYPE_ACK1:
            client_got_ack(c, msg_type);
            break;

        default:
            ret = -1;
    }

    return ret;

  error:
    return -1;
}

void usage(char *prog)
{
    printf("usage: %s [-6] [host] port\n", prog);
}

void signal_handler(int sig)
{
    switch(sig)
    {
        case SIGINT:
            running = 0;
    }
}
