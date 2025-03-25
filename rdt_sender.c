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
#include <stdbool.h>

#include "packet.h"
#include "common.h"
#include "vector.h"


void start_timer(void);
void stop_timer(void);
void resend_packets(int sig);
void init_timer(int delay, void (*sig_handler)(int));


// RTT and RTO-related constants
#define INITIAL_RTO 3000        // Initial RTO in milliseconds (3 seconds)
#define MAX_RTO 6000          // Maximum RTO in milliseconds (6 seconds)
#define MIN_RTO 100             // Minimum RTO in milliseconds (100 ms)
#define ALPHA 0.125             // Weight for SRTT calculation (RFC 6298 recommends 0.125)
#define BETA 0.25               // Weight for RTTVAR calculation (RFC 6298 recommends 0.25)
#define K 4                     // Multiplier for RTO calculation

// Congestion control constants
#define INITIAL_SSTHRESH 64     // Initial slow start threshold in packets

// Congestion control states
#define SLOW_START 0
#define CONGESTION_AVOIDANCE 1

// Log file name
#define CSV_FILENAME "CWND.csv"

// Packet timestamp structure to track when each packet was sent
typedef struct {
    int seqno;                  // Sequence number of the packet
    struct timeval send_time;   // Time when packet was sent
    bool retransmitted;         // Flag to track if packet was retransmitted
} packet_timestamp;

// Function prototypes for RTT and RTO calculation
void update_rtt(int seqno, struct timeval *send_time, bool was_retransmitted);
void calculate_rto(void);
int get_current_rto(void);
void record_packet_sent(int seqno, bool is_retransmit);
struct timeval* get_packet_send_time(int seqno);
bool was_packet_retransmitted(int seqno);

// Function prototypes for congestion control
void update_congestion_window(bool ack_received, bool timeout, bool triple_dup_ack);
void log_congestion_state(void);
void log_to_csv(void); // New function to log to CSV

#define STDIN_FD    0
#define RETRY  120 
#define MAX_WINDOW_SIZE 100 // Maximum window size allowed
#define MAX_TIMESTAMPS 1000 // Maximum number of timestamps to track

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
int last_ack_received = -1;     // Track last ACK for better duplicate detection

// RTT and RTO related variables
packet_timestamp timestamps[MAX_TIMESTAMPS];  // Array to store packet timestamps
int timestamp_count = 0;                      // Number of timestamps recorded
int srtt = -1;                                // Smoothed RTT (initially undefined)
int rttvar = -1;                              // RTT variation (initially undefined)
int rto = INITIAL_RTO;                        // Current RTO value in milliseconds
int consecutive_timeouts = 0;                 // Count of consecutive timeouts for backoff

// Congestion control variables
int ssthresh = INITIAL_SSTHRESH;              // Slow start threshold in packets
int congestion_state = SLOW_START;            // Current congestion control state
float fractional_cwnd = 0;                    // For precise CWND calculation in CA

// CSV logging variables
FILE *csv_file = NULL;

// Get high-precision timestamp with microseconds as decimal
double get_timestamp_with_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
}

// Log to CSV with current timestamp, cwnd, and ssthresh
void log_to_csv() {
    if (csv_file != NULL) {
        double timestamp = get_timestamp_with_ms();
        
        // Use fractional_cwnd when in congestion avoidance, otherwise use vector size
        float cwnd_value = (congestion_state == CONGESTION_AVOIDANCE && fractional_cwnd > 0) 
                          ? fractional_cwnd 
                          : (float)vector_size(&packet_window);
        
        fprintf(csv_file, "%.6f,%.2f,%d\n", timestamp, cwnd_value, ssthresh);
        fflush(csv_file); // Ensure data is written immediately
    }
}

void resend_packets(int sig) //resend oldest packet
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timeout happened for segment starting at %d", send_base);
        
        // Implement exponential backoff for consecutive timeouts
        consecutive_timeouts++;
        if (consecutive_timeouts > 1) {
            rto *= 2;  // Double the RTO on consecutive timeouts
            if (rto > MAX_RTO) {
                rto = MAX_RTO;  // Cap at maximum allowed value
            }
            printf("Exponential backoff: RTO now %d ms for segment %d\n", rto, send_base);
        }
        
        // Update congestion control for timeout
        update_congestion_window(false, true, false);
        
        // Log after timeout (even though not from ACK)
        log_to_csv();
        
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
                printf("Timeout - packet resend with seqno: %d, RTO: %d ms, Segment: %d\n", 
                       oldest_packet->hdr.seqno, rto, send_base);
                
                // Record this packet as retransmitted (for Karn's algorithm)
                record_packet_sent(oldest_packet->hdr.seqno, true);
                
                if(sendto(sockfd, oldest_packet, TCP_HDR_SIZE + get_data_size(oldest_packet), 0, 
                        (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
            } else {
                printf("Warning: No packet found at index %d to resend\n", window_index);
            }
        }
        
        init_timer(rto, resend_packets);  // Update timer with current RTO
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

// Function to record packet send time for RTT calculation
void record_packet_sent(int seqno, bool is_retransmit) 
{
    int i;
    
    // First check if we already have an entry for this sequence number
    for (i = 0; i < timestamp_count; i++) {
        if (timestamps[i].seqno == seqno) {
            // Update existing entry
            gettimeofday(&timestamps[i].send_time, NULL);
            timestamps[i].retransmitted = is_retransmit;
            return;
        }
    }
    
    // If we reach MAX_TIMESTAMPS, start overwriting from the beginning
    if (timestamp_count >= MAX_TIMESTAMPS) {
        timestamp_count = 0;
    }
    
    // Add new entry
    timestamps[timestamp_count].seqno = seqno;
    gettimeofday(&timestamps[timestamp_count].send_time, NULL);
    timestamps[timestamp_count].retransmitted = is_retransmit;
    timestamp_count++;
}

// Get the send time for a packet with given sequence number
struct timeval* get_packet_send_time(int seqno) 
{
    for (int i = 0; i < MAX_TIMESTAMPS; i++) {
        if (timestamps[i].seqno == seqno) {
            return &timestamps[i].send_time;
        }
    }
    return NULL;
}

// Check if a packet was retransmitted (for Karn's algorithm)
bool was_packet_retransmitted(int seqno) 
{
    for (int i = 0; i < MAX_TIMESTAMPS; i++) {
        if (timestamps[i].seqno == seqno) {
            return timestamps[i].retransmitted;
        }
    }
    return false;
}

// Calculate RTT for an acknowledged packet and update RTO
void update_rtt(int ackno, struct timeval *send_time, bool was_retransmitted) 
{
    struct timeval now, diff;
    int rtt_ms;
    
    // Skip RTT calculation for retransmitted packets (Karn's algorithm)
    if (was_retransmitted) {
        printf("Skipping RTT calculation for retransmitted packet (Karn's algorithm)\n");
        return;
    }
    
    gettimeofday(&now, NULL);
    
    // Calculate RTT in milliseconds
    timersub(&now, send_time, &diff);
    rtt_ms = diff.tv_sec * 1000 + diff.tv_usec / 1000;
    
    printf("Measured RTT: %d ms for packet %d\n", rtt_ms, ackno);
    
    // Initialize SRTT and RTTVAR on first RTT measurement
    if (srtt == -1) {
        srtt = rtt_ms;
        rttvar = rtt_ms / 2;
        printf("Initial SRTT: %d ms, RTTVAR: %d ms\n", srtt, rttvar);
    } else {
        // Update RTTVAR: RTTVAR = (1-BETA) * RTTVAR + BETA * |SRTT - RTT|
        rttvar = (int)((1 - BETA) * rttvar + BETA * abs(srtt - rtt_ms));
        
        // Update SRTT: SRTT = (1-ALPHA) * SRTT + ALPHA * RTT
        srtt = (int)((1 - ALPHA) * srtt + ALPHA * rtt_ms);
        
        printf("Updated SRTT: %d ms, RTTVAR: %d ms\n", srtt, rttvar);
    }
    
    // Calculate new RTO: RTO = SRTT + K * RTTVAR
    rto = srtt + K * rttvar;
    
    // Ensure RTO is within bounds
    if (rto < MIN_RTO) {
        rto = MIN_RTO;
    } else if (rto > MAX_RTO) {
        rto = MAX_RTO;
    }
    
    printf("New RTO: %d ms\n", rto);
    
    // Reset consecutive timeout counter on successful ACK
    consecutive_timeouts = 0;
}

// Get the current RTO value
int get_current_rto(void) 
{
    return rto;
}

// Update congestion window based on events
// Update congestion window based on events
void update_congestion_window(bool ack_received, bool timeout, bool triple_dup_ack) 
{
    int old_state = congestion_state;
    int old_size = vector_size(&packet_window);
    
    if (timeout) {
        // On timeout: enter slow start and set ssthresh to half of current window
        ssthresh = vector_size(&packet_window) / 2;
        if (ssthresh < 2) ssthresh = 2; // Minimum ssthresh
        
        // Reset vector size to 1 for slow start
        packet_window.v_size = 1;
        congestion_state = SLOW_START;
        fractional_cwnd = 0; // Reset fractional CWND
        
        printf("TIMEOUT: window_size=%d, ssthresh=%d, state=SLOW_START\n", 
               vector_size(&packet_window), ssthresh);
    } 
    else if (triple_dup_ack) {
        // Fast Retransmit - adjust ssthresh to max(CWND/2, 2)
        int half_window = vector_size(&packet_window) / 2;
        ssthresh = (half_window > 2) ? half_window : 2; 
        
        // Set window size to 1 and enter slow start (similar to timeout)
        packet_window.v_size = 1;
        congestion_state = SLOW_START;
        fractional_cwnd = 0; // Reset fractional CWND
        
        printf("TRIPLE DUP ACK: window_size=%d, ssthresh=%d, state=SLOW_START\n", 
               vector_size(&packet_window), ssthresh);
    }
    else if (ack_received) {
        if (congestion_state == SLOW_START) {
            // In slow start: increase window exponentially by exactly 1 for each ACK
            // This ensures proper progression: 1, 2, 4, 8, etc.
            packet_window.v_size += 1;  // Increase by exactly 1 packet per ACK
            
            if (packet_window.v_size > MAX_WINDOW_SIZE) {
                packet_window.v_size = MAX_WINDOW_SIZE;
            }
            
            // If window size reaches or exceeds ssthresh, transition to congestion avoidance
            if (vector_size(&packet_window) >= ssthresh) {
                congestion_state = CONGESTION_AVOIDANCE;
                // Initialize fractional_cwnd to exactly match the current window size
                fractional_cwnd = (float)vector_size(&packet_window);
                printf("Transition: SLOW_START -> CONGESTION_AVOIDANCE at window_size=%d\n", 
                       vector_size(&packet_window));
            }
        } 
        else if (congestion_state == CONGESTION_AVOIDANCE) {
            // In Congestion Avoidance: increase CWND by exactly 1/CWND for each ACK
            // This ensures a linear growth of approximately 1 packet per RTT
            if (fractional_cwnd == 0) {
                // Initialize on first entry to CA
                fractional_cwnd = (float)vector_size(&packet_window);
            }
            
            // Increase by 1/CWND for this ACK
            fractional_cwnd += 1.0 / fractional_cwnd;
            
            // Take the floor value for the actual window size
            int new_window_size = (int)fractional_cwnd; // Floor value
            
            // Update window size if it has changed
            if (new_window_size > vector_size(&packet_window)) {
                packet_window.v_size = new_window_size;
                if (packet_window.v_size > MAX_WINDOW_SIZE) {
                    packet_window.v_size = MAX_WINDOW_SIZE;
                    fractional_cwnd = MAX_WINDOW_SIZE; // Cap fractional value too
                }
                printf("CONGESTION_AVOIDANCE: Incremented window to %d (fractional: %.2f)\n", 
                       vector_size(&packet_window), fractional_cwnd);
            }
        }
    }
    
    // Log congestion state changes for debugging
    if (old_state != congestion_state || old_size != vector_size(&packet_window)) {
        log_congestion_state();
    }
}

// Log current congestion control state
void log_congestion_state(void) 
{
    const char* state_str;
    
    switch (congestion_state) {
        case SLOW_START:
            state_str = "SLOW_START";
            break;
        case CONGESTION_AVOIDANCE:
            state_str = "CONGESTION_AVOIDANCE";
            break;
        default:
            state_str = "UNKNOWN";
    }
    
    printf("Congestion Control: state=%s, window_size=%d, ssthresh=%d\n", 
           state_str, vector_size(&packet_window), ssthresh);
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
    
    // Initialize packet window with MAX_WINDOW_SIZE
    vector_init(&packet_window, MAX_WINDOW_SIZE);
    
    // Initialize congestion control - set window size to 1 for slow start
    packet_window.v_size = 1;  
    ssthresh = INITIAL_SSTHRESH;
    congestion_state = SLOW_START;
    
    // Initialize CSV logging
    csv_file = fopen(CSV_FILENAME, "w");
    if (csv_file == NULL) {
        fprintf(stderr, "Warning: Could not open CSV file for logging: %s\n", CSV_FILENAME);
    } else {
        // Log initial state
        log_to_csv();
    }
    
    // Log initial congestion control state
    log_congestion_state();

    // Initialize RTT and RTO variables
    srtt = -1;
    rttvar = -1;
    rto = INITIAL_RTO;
    consecutive_timeouts = 0;
    
    // Initialize timer with initial RTO instead of RETRY
    init_timer(rto, resend_packets);
    next_seqno = 0;
    send_base = 0;
    
    printf("Starting with initial RTO: %d ms\n", rto);
    
    while (1) {
        // when eof is acked exit
        if (eof_acked) {
            printf("EOF packet has been ack'd. Exiting.\n");
            break;
        }
        
        // Dynamic window size controlled by congestion control
        int current_window_size = vector_size(&packet_window);
        
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
            
            // Update vector size if needed but don't exceed congestion window size
            if (window_index >= vector_size(&packet_window)) {
                // The actual size is determined by congestion control, so we don't update here
            }
            
            VLOG(DEBUG, "Sending packet %d to %s (Window size: %d, RTO: %d ms, State: %s)", 
                next_seqno, inet_ntoa(serveraddr.sin_addr), current_window_size, rto,
                congestion_state == SLOW_START ? "SLOW_START" : "CONGESTION_AVOIDANCE");
            
            // Record packet sent time for RTT calculation (not retransmitted)
            record_packet_sent(next_seqno, false);
            
            // send packet 
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + len, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            
            // start timer for first packet 
            if(next_seqno == send_base) {
                init_timer(rto, resend_packets);  // Use current RTO value
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
            init_timer(rto, resend_packets);  // Use current RTO value
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
            last_ack_received = recvpkt->hdr.ackno; // Update last ACK received
            
            // Log to CSV immediately when we receive a new ACK (not duplicate)
            // This logs CWND info when ACK is received, before updating CWND
            log_to_csv();
            
            // Process acknowledged packets
            int last_acknowledged = send_base;
            
            // free ack'd packet and update send base
            while(send_base < recvpkt->hdr.ackno) {
                int window_index = (send_base / DATA_SIZE) % vector_capacity(&packet_window);
                tcp_packet* packet_to_free = vector_at(&packet_window, window_index);
                if(packet_to_free != NULL) {
                    // For the last packet being freed, update RTT if not retransmitted
                    if (send_base == last_acknowledged) {
                        struct timeval* send_time = get_packet_send_time(send_base);
                        bool retransmitted = was_packet_retransmitted(send_base);
                        
                        if (send_time != NULL) {
                            update_rtt(send_base, send_time, retransmitted);
                        }
                    }
                    
                    free(packet_to_free);
                    packet_window.data[window_index] = NULL;
                    
                    // Update congestion window for this ACK
                    update_congestion_window(true, false, false);
                }
                
                last_acknowledged = send_base;
                
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
                init_timer(rto, resend_packets);  // Use current RTO value
                start_timer();
            } else {
                stop_timer();
            }
        } else if (recvpkt->hdr.ackno == send_base && recvpkt->hdr.ackno != last_ack_received) {
            // First time seeing this ACK that matches send_base (not a duplicate)
            last_ack_received = recvpkt->hdr.ackno;
            
            // Log to CSV for this non-duplicate ACK as well
            log_to_csv();
            
            // Process as regular ACK...
            // (In this case, we don't need to do anything - just avoid counting as duplicate)
        } else {
            // dupe ack received
            VLOG(INFO, "Duplicate ACK received: %d", recvpkt->hdr.ackno);
            previous_acks[acknum % 3] = recvpkt->hdr.ackno; // add it to dupe ack array
            acknum++;
            
            // Check for triple duplicate ACKs
            if (acknum >= 3 && previous_acks[0] == previous_acks[1] && previous_acks[1] == previous_acks[2] && previous_acks[0] != -1) {
                
                VLOG(INFO, "3 Duplicate ACKs detected - Fast retransmit"); 
                
                // Update congestion control for triple duplicate ACK
                update_congestion_window(false, false, true);
                
                // Log after triple duplicate ACK
                log_to_csv();
                
                // fast retransmit the packet
                int window_index = (send_base / DATA_SIZE) % vector_capacity(&packet_window);
                tcp_packet* retransmit_packet = vector_at(&packet_window, window_index);
                
                if (retransmit_packet != NULL) { // send oldest packet again
                    printf("Fast retransmitting packet with seqno: %d\n", retransmit_packet->hdr.seqno);
                    
                    // Mark packet as retransmitted for Karn's algorithm
                    record_packet_sent(retransmit_packet->hdr.seqno, true);
                    
                    if(sendto(sockfd, retransmit_packet, TCP_HDR_SIZE + get_data_size(retransmit_packet), 0, 
                            (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
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
        printf("Current status - Window: %d packets, ssthresh: %d, state: %s, Next Seq: %d, Base: %d, RTO: %d ms\n", 
              vector_size(&packet_window), ssthresh, 
              congestion_state == SLOW_START ? "SLOW_START" : "CONGESTION_AVOIDANCE",
              next_seqno, send_base, rto);
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
    
    // Close CSV file
    if (csv_file != NULL) {
        fclose(csv_file);
        printf("CSV log file saved to: %s\n", CSV_FILENAME);
    }
    
    return 0;
}