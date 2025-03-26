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

// declaring the timers to start stop and initialize the timers for packer retrasmitting
void start_timer(void);
void stop_timer(void);
void resend_packets(int sig); //signal handler
void init_timer(int delay, void (*sig_handler)(int));

//defining the constants for the RTT AND RTO
#define INITIAL_RTO 3000        // rto is initially 3 seconds
#define MAX_RTO 6000          //rto will reach a max of 6 secs
#define MIN_RTO 100             // rto min is 100 ms
#define ALPHA 0.125             // alpha value as suggested in the slides (recommends 0.125)
#define BETA 0.25               // beta value as suggested in the slides  (recommends 0.25)
#define K 4                     // the multiplier for final rto 


#define INITIAL_SSTHRESH 64    //initialzing the ssthresh to 64 pkts
#define SLOW_START 0 // the two states that we can have 0 being slow start and 1 being congestion avoidance 
#define CONGESTION_AVOIDANCE 1

#define CSV_FILENAME "CWND.csv" //in order to log the chanegs in the cwnd

// defining a struct for the the pkt time stamp to know when each pkt is being sent
typedef struct {
    int seqno;                  // seq num
    struct timeval send_time;   // time pkt sent
    bool retransmitted;         // boolean to know if the pkt is new or a retransmission (will be used later to skip in rtt calc as per karns algorithm)
} packet_timestamp;

//measuring rtt and rto dunctions
void update_rtt(int seqno, struct timeval *send_time, bool was_retransmitted); //updating the calc of rtt based on the acks we are receving 
void calculate_rto(void); // computing new rto based on the rtt changes 
//record and get the pkt timestamps 
int get_current_rto(void);
void record_packet_sent(int seqno, bool is_retransmit);
struct timeval* get_packet_send_time(int seqno);
bool was_packet_retransmitted(int seqno);

//managing congestion control
void update_congestion_window(bool ack_received, bool timeout, bool triple_dup_ack); //adjusting cwnd based on the possible events (getting an ack, a timeout, or 3 dup acks)
void log_congestion_state(void);
void log_to_csv(void); // logging thr functions to track the congestion state

#define STDIN_FD    0
#define RETRY  120  //defining a retry limit in order not to go into an infinite loop
#define MAX_WINDOW_SIZE 100 // max window size
#define MAX_TIMESTAMPS 1000 // max timestamps for tracking

int next_seqno=0; //initially zero increment for each pkt
int send_base=0; //initially zero increments with acks
Vector packet_window; // from defined vector (check vector.c, vector.h )
int sockfd, serverlen; //socket file descriptor for network communication + the length of the server address struct
struct sockaddr_in serveraddr; //carries the IP address and port number of dest
struct itimerval timer; //setup and manage timeout intervals
tcp_packet *sndpkt; //points to the pkt thats currently being sent
tcp_packet *recvpkt;//points to the received pkts/ acks
sigset_t sigmask; //responsible for making sure that timers wont interupt other operations 
int previous_acks[3] = {-1, -1, -1}; //array to detect 3 sup acks
int acknum = 0;//ack counter
int packet_count = 0;//total pkts sent 
int eof_reached = 0;       // eof reached
int eof_packet_sent = 0;   // eof sent
int eof_acked = 0;         // eof acked
tcp_packet *eof_packet = NULL;  // eof packet ptr
int last_ack_received = -1;     // track last ack for better duplicate detection

//variables to track the rtt and rto
packet_timestamp timestamps[MAX_TIMESTAMPS];  //array storing the timestamps for the pkts that are snt out
int timestamp_count = 0;                      // num of timestamps recorded
int srtt = -1;                                // initially not defined, smoothed RTT which is calculated by srtt = (1-ALPHA) * srtt + ALPHA * measured_rtt
int rttvar = -1;                              //  initially not defined, the rtt deviation
int rto = INITIAL_RTO;                        
int consecutive_timeouts = 0;                 // counting the number of consecutive timeouts for the exponential backoff

// variables for congestion control
int ssthresh = INITIAL_SSTHRESH;              // slow start thresh
int congestion_state = SLOW_START;            // initially the state will be at slow start, later can move to congestion avoidance
float fractional_cwnd = 0;                    // float definition to allow the later fractional increment (+=1/cwnd) in congestion avoidance

FILE *csv_file = NULL;

// getting a more precise timestamp (microsec as decimal)
double get_timestamp_with_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
}

//logging all the congetion control details into a csv file
void log_to_csv() {
    if (csv_file != NULL) { //check if file is succesfully opened beforfe proceeding 
        double timestamp = get_timestamp_with_ms(); //get precise current timestamp 
        //check if we are currently at slow start or congestion avoidance to determine whether to increment fractionally or not
        float cwnd_value = (congestion_state == CONGESTION_AVOIDANCE && fractional_cwnd > 0) 
                          ? fractional_cwnd 
                          : (float)vector_size(&packet_window);
        //immediately writing to the file 
        fprintf(csv_file, "%.6f,%.2f,%d\n", timestamp, cwnd_value, ssthresh);
        fflush(csv_file); 
    }
}

void resend_packets(int sig) //resend oldest packet
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timeout happened for segment starting at %d", send_base); 
        
        // exponential back off 
        consecutive_timeouts++;
        if (consecutive_timeouts > 1) {
            rto *= 2;  // more than 1 timeout for pkt consecutively, double the rto
            if (rto > MAX_RTO) { //making sure to limtit the rto to the max which is 240 seconds
                rto = MAX_RTO;  
            }
            printf("Exponential backoff: RTO now %d ms for segment %d\n", rto, send_base);
        }
        
        update_congestion_window(false, true, false); //updating the cwnd afer timeout
    
        log_to_csv(); //logging to the csv

        if (eof_reached && eof_packet_sent && !eof_acked) { // this handles the case if we reached eof, and it was sent but not acked
            printf("Timeout - eof packet resend\n");
            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) { //resenf the eof pkt
                error("sendto");
            }
        } else { // this handles the typical case, so oldest pkt is being sent
            int window_index = (send_base/DATA_SIZE) % vector_capacity(&packet_window); //calc the index of the oldest unacked pkt
            tcp_packet* oldest_packet = vector_at(&packet_window, window_index); 
            if (oldest_packet != NULL) { 
                printf("Timeout - packet resend with seqno: %d, RTO: %d ms, Segment: %d\n", 
                       oldest_packet->hdr.seqno, rto, send_base);
            
                record_packet_sent(oldest_packet->hdr.seqno, true); //making sure to mark this as retransmission to skip during implementation of karns algorithm
                
                if(sendto(sockfd, oldest_packet, TCP_HDR_SIZE + get_data_size(oldest_packet), 0, 
                        (const struct sockaddr *)&serveraddr, serverlen) < 0) { //handling error case of returing a negative value which means that there was a network related error  
                    error("sendto");
                }
            } else { // handling the error case of a missing packets
                printf("Warning: No packet found at index %d to resend\n", window_index);
            }
        }
        
        init_timer(rto, resend_packets);  // updating timer to current rto
        start_timer(); // restart timer on oldest packet
    }
}


void start_timer() 
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL); //unnlocking any signal the was set in sigmask
    setitimer(ITIMER_REAL, &timer, NULL); // starting a timer to genertae SIGALRM signals only for when the timer expires
}


void stop_timer() 
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL); //here we block the signals to stop the SIGALRM from interupting the process until its unlocked again
}


/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler); //calling sig_handler for the SIGALRM signal is received 
    timer.it_interval.tv_sec = delay / 1000; //2nd part of the timer also converting ms to secs  
    timer.it_interval.tv_usec = (delay % 1000) * 1000;   //get remainder of the delay by using modulo 1000 then cnvert to microsecs
    timer.it_value.tv_sec = delay / 1000; 
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);//clear any previos signal settings
    sigaddset(&sigmask, SIGALRM);//used to control when the timer can interrup the start/stop timer
}

void record_packet_sent(int seqno, bool is_retransmit) 
{
    int i;
    
//check for existing enteries
    for (i = 0; i < timestamp_count; i++) { //loop over the existing timestamps
        if (timestamps[i].seqno == seqno) { //if we find a matched seq num
            gettimeofday(&timestamps[i].send_time, NULL);//update the send time to the current time using gettimeofday()
            timestamps[i].retransmitted = is_retransmit;// setting the retransmission flag if needed
            return;
        }
    }
    
    if (timestamp_count >= MAX_TIMESTAMPS) { //check if we reached max time stamp
        timestamp_count = 0;//if yes, reset the counter to 0 
    }

    timestamps[timestamp_count].seqno = seqno; //store seq num of pkt
    gettimeofday(&timestamps[timestamp_count].send_time, NULL); //record current time
    timestamps[timestamp_count].retransmitted = is_retransmit;//setting the restransmison flag to "is_retrasnmit"
    timestamp_count++; //incrementing the counter
}


struct timeval* get_packet_send_time(int seqno) //returns a pointer to the struct timeval
{
    for (int i = 0; i < MAX_TIMESTAMPS; i++) { //loop over all the possible timestamps
        if (timestamps[i].seqno == seqno) { //check if the current timestamp entry matches with the resquested seq num
            return &timestamps[i].send_time; //if matched, return its memory address
        }
    }
    return NULL;//otherwise return null as there is no send time info available
}


bool was_packet_retransmitted(int seqno) //checking for retransmitted packets to ignore them in calculating rtt measurments (karns algorithm)
{
    for (int i = 0; i < MAX_TIMESTAMPS; i++) { //similar to the above block
        if (timestamps[i].seqno == seqno) {
            return timestamps[i].retransmitted;
        }
    }
    return false; //returning false assuming that pkt not found in the timestamp array has not been retransmitted
}



void update_rtt(int ackno, struct timeval *send_time, bool was_retransmitted) //function for updating the RTT based on akcs received 
{
    struct timeval now, diff; //variables for current time and rtt difference
    int rtt_ms; //rtt val in ms
    
    if (was_retransmitted) { //ignore rtt calc from retrasnmitted packets to implement karns algorthm
        printf("Skipping RTT calculation for retransmitted packet (Karn's algorithm)\n");
        return;
    }
    
    gettimeofday(&now, NULL);
    
    timersub(&now, send_time, &diff); //diff = now - send_time
    rtt_ms = diff.tv_sec * 1000 + diff.tv_usec / 1000; //convert to milli secs
    
    printf("Measured RTT: %d ms for packet %d\n", rtt_ms, ackno);
    
    if (srtt == -1) {
        srtt = rtt_ms; //initialize the smooth rtt to the first rtt sample
        rttvar = rtt_ms / 2; //rtt variation is initalliy firstmeasurement/2
        printf("Initial SRTT: %d ms, RTTVAR: %d ms\n", srtt, rttvar);
    } else {
       //updating smooth rtt and rtt variation by using Exponential Weighted Moving Average
        rttvar = (int)((1 - BETA) * rttvar + BETA * abs(srtt - rtt_ms)); 
        srtt = (int)((1 - ALPHA) * srtt + ALPHA * rtt_ms);
        
        printf("Updated SRTT: %d ms, RTTVAR: %d ms\n", srtt, rttvar);
    }
    rto = srtt + K * rttvar; //calculating rto using the formula mentioned in the slides, where k=4
//making sure rto is within the limit
    if (rto < MIN_RTO) {
        rto = MIN_RTO;
    } else if (rto > MAX_RTO) { 
        rto = MAX_RTO;
    }
    
    printf("New RTO: %d ms\n", rto);

    consecutive_timeouts = 0; //upon getting a valid ack. reset the consecutive timeout counter to 0 to record the next consecutive timeout
}

int get_current_rto(void) //get current rto value
{
    return rto;
}


void update_congestion_window(bool ack_received, bool timeout, bool triple_dup_ack) //function to update the congestion window based on the 3 possible network events: 1- normal ack received, 2- timeout, 3- 3 dupe acks

{
    int old_state = congestion_state; //before any adjustments the current congestion state and window are stored
    int old_size = vector_size(&packet_window);
    
    if (timeout) { //upon timeout
        ssthresh = vector_size(&packet_window) / 2;// ssthresh is half the current window
        if (ssthresh < 2) ssthresh = 2;  //enforcing a min ssthresh of 2
        
        packet_window.v_size = 1; //vector size=1 for slow start 
        congestion_state = SLOW_START; //state is changed to slow start 
        fractional_cwnd = 0; // reset to 0 , to be used later when state = congestion avoidance
        
        printf("TIMEOUT: window_size=%d, ssthresh=%d, state=SLOW_START\n", 
               vector_size(&packet_window), ssthresh);
    } 
    else if (triple_dup_ack) { //in the case of 3 duplicate acks
        int half_window = vector_size(&packet_window) / 2; //ssthresh is current window halfed
        ssthresh = (half_window > 2) ? half_window : 2;  //enforcing min ssthresh of 2
        
        packet_window.v_size = 1;//window size is set to 1 
        congestion_state = SLOW_START; //starts slow start phase
        fractional_cwnd = 0; 
        
        printf("TRIPLE DUP ACK: window_size=%d, ssthresh=%d, state=SLOW_START\n", 
               vector_size(&packet_window), ssthresh);
    }
    else if (ack_received) { //normal ack case
        if (congestion_state == SLOW_START) {
            packet_window.v_size += 1;  // the window is incremented by one per ack received, and if all packets in window acked, the window will double for each rtt
            
            if (packet_window.v_size > MAX_WINDOW_SIZE) { //forcing a max window size to not overflow the buffer 
                packet_window.v_size = MAX_WINDOW_SIZE;
            }
            
            if (vector_size(&packet_window) >= ssthresh) {//checking if the window size reached the ssthresh
                congestion_state = CONGESTION_AVOIDANCE; //enter congestion avoidance if so
                fractional_cwnd = (float)vector_size(&packet_window); //initialzing the fractional cwnd so we can accept icrements by +=1/cwnd
                printf("Transition: SLOW_START -> CONGESTION_AVOIDANCE at window_size=%d\n", 
                       vector_size(&packet_window));
            }
        } 
        else if (congestion_state == CONGESTION_AVOIDANCE) {//handling for congestion avoidance phase
            if (fractional_cwnd == 0) { //if fractiona cwnd is not already initialized
                fractional_cwnd = (float)vector_size(&packet_window); //initialize
            }

            fractional_cwnd += 1.0 / fractional_cwnd;// for each ack fractional cwn is incremented by 1/cwnd 
   
            int new_window_size = (int)fractional_cwnd; // consider floor value

            if (new_window_size > vector_size(&packet_window)) { //if there was an integer increment update the value of the window size
                packet_window.v_size = new_window_size;
                if (packet_window.v_size > MAX_WINDOW_SIZE) {
                    packet_window.v_size = MAX_WINDOW_SIZE;
                    fractional_cwnd = MAX_WINDOW_SIZE; 
                }
                printf("CONGESTION_AVOIDANCE: Incremented window to %d (fractional: %.2f)\n", 
                       vector_size(&packet_window), fractional_cwnd);
            }
        }
    }
//in the case that either congestion state was changed, or window size was changed log it
    if (old_state != congestion_state || old_size != vector_size(&packet_window)) {
        log_congestion_state(); //calling the logging 
    }
}




void log_congestion_state(void) //to log the congestion state 
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
    printf("Congestion Control: state=%s, window_size=%d, ssthresh=%d\n", //logging the current congestion control state, window size, and ssthresh
           state_str, vector_size(&packet_window), ssthresh);
}

int main (int argc, char **argv)
{
    int portno, len;//declaring the port number of the server, and len
    char *hostname; //to save the server hostname
    char buffer[DATA_SIZE]; //buffer to read from file
    FILE *fp; //pointer to read the input files

    if (argc != 4) { //checks if the 4 arguements are passed in (program name, hostname, port, filename)
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1]; //extracting the arguements and saving them in the appropriate variable
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) { //checking if file operning is successful 
        error(argv[3]);
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0); //creating udp socket and checking for errors in socket creation
    if (sockfd < 0) 
        error("ERROR opening socket");

    bzero((char *) &serveraddr, sizeof(serveraddr)); //initially the server address struct is set to zeros
    serverlen = sizeof(serveraddr);//to store the length of the server address struct

    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) { //conversion of  hostname string to ip add
        fprintf(stderr,"ERROR, invalid host %s\n", hostname); //checking for an invalid hostname
        exit(0);
    }

    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0); //checking if there is room for data in pkts
    
    vector_init(&packet_window, MAX_WINDOW_SIZE); //initializing the packet window vector with the max cap
    packet_window.v_size = 1;  //initial congestion control params, window size=1
    ssthresh = INITIAL_SSTHRESH; // starting with the inital slow start thresh from declared constant INITIAL_SSTHRESH
    congestion_state = SLOW_START; // state starts as slow start initially 
    

    csv_file = fopen(CSV_FILENAME, "w"); //opening and writing to the csv file to log the congestion window changes 
    if (csv_file == NULL) {// if file opening fails 
        fprintf(stderr, "Warning: Could not open CSV file for logging: %s\n", CSV_FILENAME); //warn
    } else {
        log_to_csv(); //otherwise log the initial state
    }
    
    log_congestion_state(); //log the initial congestion control state
//initially sinxe smooth rtt and rttvar are not calculated yet
    srtt = -1; //initially set to -1
    rttvar = -1;//initially set to -1
    rto = INITIAL_RTO;//set to 3 secs
    consecutive_timeouts = 0; //resetting the counter that checks for consecutive timouts to check for new timeouts
    

    init_timer(rto, resend_packets); //initializing the timer with params :the rto value, and the call back function for expired timer
    next_seqno = 0;
    send_base = 0;
    
    printf("Starting with initial RTO: %d ms\n", rto);
    
    while (1) { 

        if (eof_acked) { //if we reached an eof pkt and it is acked, print message abd break the loop
            printf("EOF packet has been ack'd. Exiting.\n");
            break;
        }
        
  
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
            
            packet_window.data[window_index] = sndpkt; //store new packet in the window
            

            if (window_index >= vector_size(&packet_window)) {
                // placeholder- the actual size is determined by congestion control, so we don't update here
            }
            
            VLOG(DEBUG, "Sending packet %d to %s (Window size: %d, RTO: %d ms, State: %s)", 
                next_seqno, inet_ntoa(serveraddr.sin_addr), current_window_size, rto,
                congestion_state == SLOW_START ? "SLOW_START" : "CONGESTION_AVOIDANCE");
            

            record_packet_sent(next_seqno, false); //record the time that the pkt was sent to use later for rtt calculation, and also marking false as it its not a restransmission
            
            // send packet 
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + len, 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            
            // start timer for first packet 
            if(next_seqno == send_base) {
                init_timer(rto, resend_packets);  //current rto value
                start_timer();
            }
            
            // move next sequence number by data size
            next_seqno += len;
            packet_count++; 
        }
        
        // if all data has been acked and eof packet hasn't been sent but has been reached
        if (eof_reached && !eof_packet_sent && send_base >= next_seqno) {
            printf("All data acknowledged, sending EOF packet\n");
            if(sendto(sockfd, eof_packet, TCP_HDR_SIZE, 0,  // send eof packet and mark it as sent
                    (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            eof_packet_sent = 1;
            init_timer(rto, resend_packets);  // Use current RTO value
            start_timer(); // eof packet timer
        }
        
        
        // receive acks from server
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0) {//if ack receiving is failed, skip to next iteration 
            continue;
        }
        
        recvpkt = (tcp_packet *)buffer;
        printf("ACK RECEIVED: %d (send_base: %d)\n", 
               recvpkt->hdr.ackno, 
               send_base);
        
        // check if ack is for eof (FIN FLAG) so it doesn't mix up with dupe acks of the last packet
        if (eof_packet_sent && recvpkt->hdr.ackno >= next_seqno && recvpkt->hdr.ctr_flags==FIN) {
            printf("Received ACK for EOF packet\n"); // 
            eof_acked = 1;//mark as acked
            stop_timer(); //stop timer
            continue; // to next iteration
        }
        
        if(recvpkt->hdr.ackno > send_base) { // if ack is new
            previous_acks[0] = previous_acks[1] = previous_acks[2] = -1; // reset dupe ack array
            acknum = 0;
            last_ack_received = recvpkt->hdr.ackno; // update last ack tracker

            log_to_csv(); //log to csv immediately 
            

            int last_acknowledged = send_base;
            
            // free ack'd packet and update send base
            while(send_base < recvpkt->hdr.ackno) {
                int window_index = (send_base / DATA_SIZE) % vector_capacity(&packet_window); //calculating the index position in the pkt window where the packet is stored
                tcp_packet* packet_to_free = vector_at(&packet_window, window_index); //ge tthe pointer to the packet at the calculated window index
                if(packet_to_free != NULL) { //check if there is an existing packet at this position
                    if (send_base == last_acknowledged) { // check if the current packet is the last acked packet that was recorded before processign current ack, to consider for rtt calculaiton
                        struct timeval* send_time = get_packet_send_time(send_base); //timestamo for when the packet was sent
                        bool retransmitted = was_packet_retransmitted(send_base); //checking if packet was retransmitted
                        
                        if (send_time != NULL) {  //update rtt calcuation but check if send time isnt null first
                            update_rtt(send_base, send_time, retransmitted);
                        }
                    }
                    
                    free(packet_to_free); //free the memory allocated for the pkt since its acked now
                    packet_window.data[window_index] = NULL;
                    
                
                    update_congestion_window(true, false, false); //update congestion window (new ack->true, not a timeout->false, and not a triple duplicate ACK->false)
                }
                
                last_acknowledged = send_base; //update to the curr val of send base before incrementng 
                
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
                init_timer(rto, resend_packets);  // use current rto value
                start_timer();
            } else {
                stop_timer();
            }
        } else if (recvpkt->hdr.ackno == send_base && recvpkt->hdr.ackno != last_ack_received) {//in the case that the received ack number is equal to the oldest unacked packet, and this ack number is not the same as the last ack, then
            last_ack_received = recvpkt->hdr.ackno;//track the ack
        
            log_to_csv(); //log the current state to the csv file
            
        } else { //in all other cases, which is the dupe ack case
            VLOG(INFO, "Duplicate ACK received: %d", recvpkt->hdr.ackno); //log
            previous_acks[acknum % 3] = recvpkt->hdr.ackno; // store the ack number in a looped buffer of size 3 using modulo
            acknum++; //increment the ack trackign varaible
            
            if (acknum >= 3 && previous_acks[0] == previous_acks[1] && previous_acks[1] == previous_acks[2] && previous_acks[0] != -1) { //check for the case of three dupe acks
                
                VLOG(INFO, "3 Duplicate ACKs detected - Fast retransmit");  //log
    
                update_congestion_window(false, false, true); //(not new ack, not a timeout, is a tripple dupe ack)
               
                log_to_csv();//log to the csv
                
                // fast retransmit the packet
                int window_index = (send_base / DATA_SIZE) % vector_capacity(&packet_window); //calculates the window index for the packet that needs to be retransmitted
                tcp_packet* retransmit_packet = vector_at(&packet_window, window_index); //retreive pointer to the packet that needs to be retransmitted 
                
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
