#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"
#include "vector.h"


void start_timer(void);
void stop_timer(void);
void resend_packets(int sig);
void init_timer(int delay, void (*sig_handler)(int));


#define STDIN_FD    0
#define RETRY  120 
#define MAX_WINDOW_SIZE 100 // Maximum window size allowed

int next_seqno=0;
int send_base=0;
Vector packet_window; // Window vector
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
int previous_acks[3] = {-1, -1, -1}; 
int acknum = 0;
int packet_count = 0;
int eof_reached = 0;       // eof reached
int eof_packet_sent = 0;   // eof sent
int eof_acked = 0;         // eof acked
tcp_packet *eof_packet = NULL;  // eof packet ptr

void resend_packets(int sig) //resend oldest packet
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timeout happened");
        
        // resend eof packet if needed
        if (eof_reached && eof_packet_sent && !eof_acked) {
            printf("Timeout - eof packet resend\n");
            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
        } else {
            // send oldest packet normally
            int window_index = (send_base/DATA_SIZE) % vector_capacity(&packet_window);
            tcp_packet* oldest_packet = vector_at(&packet_window, window_index); 
            if (oldest_packet != NULL) {
                printf("Timeout - packet resend with seqno: %d\n", oldest_packet->hdr.seqno);
                if(sendto(sockfd, oldest_packet, TCP_HDR_SIZE + get_data_size(oldest_packet), 0, 
                        (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
            } else {
                printf("Warning: No packet found at index %d to resend\n", window_index);
            }
        }
        
        start_timer(); // restart timer on oldest packet
    }
}


void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;   
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;     
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);
    
    // Initialize packet window with MAX_WINDOW_SIZE (10)
    vector_init(&packet_window, MAX_WINDOW_SIZE);
    
    // Dynamic window size - start small and can grow up to MAX_WINDOW_SIZE
    int current_window_size = 1; // Start with window size of 1

    // Stop and wait protocol with dynamic window
    init_timer(RETRY, resend_packets);
    next_seqno = 0;
    send_base = 0;
    
    while (1) {
        // when eof is acked exit
        if (eof_acked) {
            printf("EOF packet has been ack'd. Exiting.\n");
            break;
        }
        
        // Calculate current window size based on vector size, but cap at MAX_WINDOW_SIZE
        current_window_size = vector_size(&packet_window) + 1;
        if (current_window_size > MAX_WINDOW_SIZE) {
            current_window_size = MAX_WINDOW_SIZE;
        }
        
        // send if window isn't full or isn't at eof
        while (next_seqno < send_base + current_window_size * DATA_SIZE && !eof_reached) {
            len = fread(buffer, 1, DATA_SIZE, fp); // read next packet
            
            if (len <= 0) { // if eof reached
                VLOG(INFO, "End Of File has been reached");
                
                eof_packet = make_packet(0);
                eof_reached = 1;
                fclose(fp);
                
                // don't send eof packet for now, ack everything else first
                break;
            }
            
            // create new packets
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len); // copy data 
            sndpkt->hdr.seqno = next_seqno;
            
            // store in the window
            int window_index = (next_seqno / DATA_SIZE) % vector_capacity(&packet_window);
            tcp_packet* existing = vector_at(&packet_window, window_index);
            if (existing != NULL) {
                free(existing);
                packet_window.data[window_index] = NULL;
            }
            
            // Update the packet in the vector
            packet_window.data[window_index] = sndpkt;
            
            // Update vector size if needed
            if (window_index >= vector_size(&packet_window)) {
                packet_window.v_size = window_index + 1;
                if (packet_window.v_size > MAX_WINDOW_SIZE) {
                    packet_window.v_size = MAX_WINDOW_SIZE;
                }
            }
            
            VLOG(DEBUG, "Sending packet %d to %s (Window size: %d)", 
                next_seqno, inet_ntoa(serveraddr.sin_addr), current_window_size);
            
            // send packet 
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + len, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            
            // start timer for first packet 
            if(next_seqno == send_base) {
                start_timer();
            }
            
            // move next sequence number by data size
            next_seqno += len;
            packet_count++; 
        }
        
        // if all data has been acked and eof packet hasn't been sent but has been reached
        if (eof_reached && !eof_packet_sent && send_base >= next_seqno) {
            printf("All data acknowledged, sending EOF packet\n");
            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0,  // send eof packet
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            eof_packet_sent = 1;
            start_timer(); // eof packet timer
        }
        
        
        // receive acks
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0) {
            continue;
        }
        
        recvpkt = (tcp_packet *)buffer;
        printf("ACK RECEIVED: %d (send_base: %d)\n", 
               recvpkt->hdr.ackno, 
               send_base);
        
        // check if ack is for eof (FIN FLAG) so it doesn't mix up with dupe acks of the last packet
        if (eof_packet_sent && recvpkt->hdr.ackno >= next_seqno && recvpkt->hdr.ctr_flags==FIN) {
            printf("Received ACK for EOF packet\n");
            eof_acked = 1;
            stop_timer();
            continue;
        }
        
        if(recvpkt->hdr.ackno > send_base) { // if ack is new
            previous_acks[0] = previous_acks[1] = previous_acks[2] = -1; // reset dupe ack array
            acknum = 0;
            
            // free ack'd packet and update send base
            while(send_base < recvpkt->hdr.ackno) {
                int window_index = (send_base / DATA_SIZE) % vector_capacity(&packet_window);
                tcp_packet* packet_to_free = vector_at(&packet_window, window_index);
                if(packet_to_free != NULL) {
                    free(packet_to_free);
                    packet_window.data[window_index] = NULL;
                    
                    // After successful ACK, potentially increase window size
                    if (current_window_size < MAX_WINDOW_SIZE) {
                        current_window_size++;
                    }
                }
                
                // increment by full packet size
                if (send_base + DATA_SIZE <= recvpkt->hdr.ackno) {
                    send_base += DATA_SIZE; 
                } else { // or increment by size of smaller packet size
                    send_base = recvpkt->hdr.ackno; 
                }
            }
            
            // time packet on new sendbase
            if(send_base < next_seqno) {
                stop_timer();
                start_timer();
            } else {
                stop_timer();
            }
        } else {
            // dupe ack received
            VLOG(INFO, "Duplicate ACK received");
            previous_acks[acknum % 3] = recvpkt->hdr.ackno; // add it to dupe ack array
            acknum++;
            
            // Check for triple duplicate ACKs
            if (acknum >= 3 && previous_acks[0] == previous_acks[1] && previous_acks[1] == previous_acks[2] && previous_acks[0] != -1) {
                
                VLOG(INFO, "3 Duplicate ACKs detected - Fast retransmit"); 
                
                // fast retransmit the packet
                int window_index = (send_base / DATA_SIZE) % vector_capacity(&packet_window);
                tcp_packet* retransmit_packet = vector_at(&packet_window, window_index);
                
                if (retransmit_packet != NULL) { // send oldest packet again
                    printf("Fast retransmitting packet with seqno: %d\n", retransmit_packet->hdr.seqno);
                    if(sendto(sockfd, retransmit_packet, TCP_HDR_SIZE + get_data_size(retransmit_packet), 0, 
                            (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                    
                    // On packet loss detected by triple duplicate ACK, reduce window size
                    if (current_window_size > 1) {
                        current_window_size = current_window_size / 2; // Cut window size in half (TCP-like behavior)
                        if (current_window_size < 1) current_window_size = 1;
                    }
                    
                    // reset dupe ack array and ctr
                    previous_acks[0] = previous_acks[1] = previous_acks[2] = -1;
                    acknum = 0;
                } else {
                    printf("Warning: No packet found at index %d for fast retransmit\n", window_index);
                }
            }
        }
        
        // Display current window status
        printf("Current window status - Size: %d, Capacity: %d, Next Seq: %d, Base: %d\n", 
               current_window_size, vector_capacity(&packet_window), next_seqno, send_base);
    }
    
    // Free any remaining packets in the window
    for (int i = 0; i < vector_capacity(&packet_window); i++) {
        tcp_packet* packet = vector_at(&packet_window, i);
        if (packet != NULL) {
            free(packet);
        }
    }
    
    if (eof_packet != NULL) {
        free(eof_packet);
    }
    
    vector_free(&packet_window);
    
    return 0;
}