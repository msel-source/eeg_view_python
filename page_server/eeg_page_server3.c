
//    eeg_page_server3 - serves MEF 3 data to a user interface
//    Copyright (C) 2021 Mayo Foundation, Rochester MN. All rights reserved.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 
//    This code was written by many members of the Mayo Systems Electrophysiology
//    Lab, originally as a page server for a Matlab sample-based MEF 2.x viewer.
//    Adapted here to be a server for a time-based Python viewer.

/* includes */
#ifndef _WIN32
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <pthread.h>
#include <float.h>
#include <time.h>
#else
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
//#include <unistd.h>
#include <sys/stat.h>
//#include <sys/time.h>
#include <signal.h>
//#include <pthread.h>
#include <float.h>
#include <time.h>
#endif

#include "meflib.h"

/* defines */
#define N_PAGES_AHEAD	50
#define READ_INTERVAL	500000
#define HEARTBEAT_INTERVAL 2
#define DISCON_MAJOR_THRESHOLD 60 * 1000000  // 1 minute

#define RF_TIMER 1001
#define DBUG		0

/* typedefs */
typedef struct {
		si4	samps_per_page, num_chans;
		sf4	*page_data;
		sf8	secs_per_page, curr_view_sec, page_to_write_start_sec;
        si8 session_start_time;
        si8 session_end_time;
    char *password;
	} FIXED_INFO;

typedef struct {
		si1		f_name[256];
		si4		chan_idx;
		ui8		num_offset_entries, index_data_offset, maximum_block_length, number_of_samples;
		sf8		native_fs; 
		ui1		encryptionKey[240], data_encryption_used;
		FILE		*d_fp;
		//INDEX_DATA	*index_array;
    CHANNEL   *channel;
		FIXED_INFO	*fixed_info;
	} THREAD_INFO;

/* globals */
si4	read_files_flag = 1;
si4 password_needed = 0;

/* prototypes */
#ifndef _WIN32
static void *read_thread(void *argument);
static void *heartbeat_thread(void *argument);
static void set_rf_flag(int);
static ui8 update_buffer_limits();
static si4 check_fud();
static void *get_mef_channel_thread(void *argument);
static void *do_nothing_thread(void *argument);
si8 sample_for_uutc_c(si8 uutc, CHANNEL *channel);
#else
DWORD WINAPI read_thread(LPVOID argument);
DWORD WINAPI heartbeat_thread(LPVOID argument);
void CALLBACK set_rf_flag(HWND hwnd, UINT uMsg, UINT timerId, DWORD dwTime);
static ui8 update_buffer_limits();
static si4 check_fud();
DWORD WINAPI get_mef_channel_thread(LPVOID argument);
DWORD WINAPI do_nothing_thread(LPVOID argument);
si8 sample_for_uutc_c(si8 uutc, CHANNEL* channel);
#endif


void memset_int(si4 *ptr, si4 value, size_t num)
{
    si4 *temp_ptr;
    int i;
    
    if (num < 1)
        return;
    
    temp_ptr = ptr;
    for (i=0;i<num;i++)
    {
        memcpy(temp_ptr, &value, sizeof(si4));
        temp_ptr++;
    }
}

int check_block_crc(ui1* block_hdr_ptr, ui4 max_samps, ui1* total_data_ptr, ui8 total_data_bytes)
{
    ui8 offset_into_data, remaining_buf_size;
    si1 CRC_valid;
    RED_BLOCK_HEADER* block_header;
    
    offset_into_data = block_hdr_ptr - total_data_ptr;
    remaining_buf_size = total_data_bytes - offset_into_data;
    
    // check if remaining buffer at least contains the RED block header
    if (remaining_buf_size < RED_BLOCK_HEADER_BYTES)
        return 0;
    
    block_header = (RED_BLOCK_HEADER*) block_hdr_ptr;
    
    // check if entire block, based on size specified in header, can possibly fit in the remaining buffer
    if (block_header->block_bytes > remaining_buf_size)
        return 0;
    
    // check if size specified in header is absurdly large
    if (block_header->block_bytes > RED_MAX_COMPRESSED_BYTES(max_samps, 1))
        return 0;
    
    // at this point we know we have enough data to actually run the CRC calculation, so do it
    CRC_valid = CRC_validate((ui1*) block_header + CRC_BYTES, block_header->block_bytes - CRC_BYTES, block_header->block_CRC);
    
    // return output of CRC heck
    if (CRC_valid == MEF_TRUE)
        return 1;
    else
        return 0;
}


int main(int argc, const char *argv[])
{
	ui1		encryptionKey[240];

	si4		i, j, k, l, fd, num_chans = 0, samps_per_page, tot_samps_per_page = 0, password_valid=0;
    si1		data_path[1024], ps_path[1024], buff_lim_path[1024], cs_path[1024], temp_path[1024], server_info_path[1024], password_needed_path[1024], events_path[1024], discon_path[1024];
	si1		b, *c1, *c2, *c3, *c4, *header, subject_password[16], session_password[16], password[16];
    si1     events_file[1024];
    sf8		secs_per_page, curr_view_sec, first_sec_written = 0.0L, last_sec_written = 0.0L;
	sf8		fud = 0.0L, nfud = 0.0L, temp_sf8;
	ui8		flen, last_heartbeat;
	FILE	*cs_fp, *ps_fp, *t_fp, *o_fp, *bl_fp, *si_fp;
	struct	stat	sb;
	FIXED_INFO	fixed_info;
	THREAD_INFO	*thread_info = NULL;
    si1  page_dir[4096];
#ifndef _WIN32
    THREAD_INFO *heartbeat_thread_id = NULL;
	pthread_t	*thread_ids = NULL;
#else
    HANDLE thread_ids[2000];
    DWORD ThreadId;
    DWORD ThreadId2;
#endif
	void		*ret_val;
#ifndef _WIN32
	struct		itimerval rf_timer;
#endif
    CHANNEL *channel;
    si1  f_name_temp[2048][256];
    CHANNEL *temp_channel_array[2048];
    si4 old_num_chans;
    int counter;
    fprintf(stderr, "args: %d\n", argc);
    // set up mef 3 library
    (void) initialize_meflib();

    secs_per_page = 30;  // TBD does this make sense?
	
    strcpy(page_dir, argv[1]);
    
    // get password, if it exists
    fixed_info.password = NULL;
    fprintf(stderr, "args: %d\n", argc);
    if (argc > 2)
    {
        fixed_info.password = argv[2];
    }

	// set up paths
	{
		sprintf(ps_path, "%s/page_specs", page_dir);
		sprintf(temp_path, "%s/page_data", page_dir);
		sprintf(cs_path, "%s/current_sec", page_dir);
		sprintf(buff_lim_path, "%s/buffer_limits", page_dir);
        sprintf(server_info_path, "%s/server_info", page_dir);
        sprintf(password_needed_path, "%s/password_needed", page_dir);
        sprintf(events_path, "%s/events", page_dir);
        sprintf(discon_path, "%s/discon", page_dir);
#ifndef _WIN32
		while ((o_fp = fopen(temp_path, "w+")) == NULL) usleep((useconds_t) 100000);
#else
        while ((o_fp = fopen(temp_path, "wb+")) == NULL) Sleep(100);
#endif
		fixed_info.page_data = NULL;
	}
    
#ifndef _WIN32
    heartbeat_thread_id = (pthread_t *) calloc((size_t) 1, sizeof(pthread_t));
    pthread_create(heartbeat_thread_id, NULL, heartbeat_thread, (void*)(page_dir));
#else
    CreateThread(NULL, 0, heartbeat_thread, (void*)page_dir, 0, &ThreadId2);
#endif


	
#ifndef _WIN32
	// set up read_files timer
	{
		signal(SIGALRM, set_rf_flag);
		rf_timer.it_value.tv_sec     = 0;
		rf_timer.it_value.tv_usec    = READ_INTERVAL; 
		rf_timer.it_interval.tv_sec  = 0;
		rf_timer.it_interval.tv_usec = READ_INTERVAL;
		setitimer (ITIMER_REAL, &rf_timer, NULL);
	}
#else
	CreateThread(NULL, 0, set_rf_flag, (void*)NULL, 0, &ThreadId);
#endif
	
	//set up passwords and decryption key 
	{
		memset(subject_password, 0, 16); memset(session_password, 0, 16); 
		memset(password, 0, 16);
		if (DBUG) printf("init passwords\n");
	}

	last_heartbeat = time(NULL) + 10000;
    
    for (i=0;i<2048;i++)
        temp_channel_array[i] = NULL;

	// server loop
	while (1) {
		// time to read
		if (read_files_flag) {
			// check current sec file
			{
#ifndef _WIN32
				while ((cs_fp = fopen(cs_path, "r")) == NULL) usleep((useconds_t) 100000);
#else
				while ((cs_fp = fopen(cs_path, "rb")) == NULL) Sleep(100);
#endif
				fscanf(cs_fp, "%lf\n", &curr_view_sec);
				fclose(cs_fp);
				if (DBUG) printf("curr_view_sec %lf\n", curr_view_sec);
#ifndef _WIN32
				if (DBUG) printf("hb interval %ld\n", time(NULL) - last_heartbeat);
#else
                if (DBUG) printf("hb interval %lld\n", time(NULL) - last_heartbeat);
#endif
				
				if (time(NULL) - (si8)last_heartbeat > HEARTBEAT_INTERVAL) {
                    if (check_fud(ps_path, nfud)) 
                        read_files_flag = 1;
                    else
                        last_heartbeat = update_buffer_limits(buff_lim_path, first_sec_written, last_sec_written);
				}

				if (curr_view_sec < 0.0L)  // exit flag
					break;
				if ((curr_view_sec > last_sec_written) || (curr_view_sec < first_sec_written)) {
					first_sec_written = curr_view_sec;
					// last_sec_written keeps track of what data we've written so far.  It will grow by secs_per_page on
					// each iteration of the endless loop, until the buffer is filled.  It starts out lower than first_sec_written,
					// because later this iteration it will be incremented by secs_per_page, making it equal to first_sec_written.
					// Each iteration after that it will be greater.
					last_sec_written = first_sec_written - secs_per_page;
					rewind(o_fp);
				}
				fixed_info.curr_view_sec = curr_view_sec;
			}
            
			// check for new page specs
			{
            start_over:
#ifndef _WIN32
				while ((ps_fp = fopen(ps_path, "r")) == NULL) usleep((useconds_t) 100000); //page_specs file
#else
				while ((ps_fp = fopen(ps_path, "r")) == NULL) Sleep(100); //page_specs file
#endif
                fscanf(ps_fp, "%lf\n", &fud); //fud = random fp number, tells if file was rewritten. No meaning beyond that
				if (fud != nfud) {
					nfud = fud;
                    
                    
                    // check for new file list (if  file list is the same, then no need to reload MEF files
                    // (unless you care about real-time data, ie. ever-growing data files
                    
                    // read base data folder
                    {
                        c1 = data_path;
                        while ((b = (si1) fgetc(ps_fp)) != '\n')
                            *c1++ = b;
                        *c1 = 0;
                    }
                    
                    old_num_chans = num_chans;
                    
                    // read number of channels
                    {
                        fscanf(ps_fp, "%d\n", &num_chans);
                        fixed_info.num_chans = num_chans;
                        if (DBUG) printf("num_chans %d\n", num_chans);
                    }
                    
                    
                    // read file names
                    {
                        for (i = 0; i < num_chans; ++i) {
                            c1 = f_name_temp[i];
                            counter = 0;
                            while ((b = (si1) fgetc(ps_fp)) != '\n')
                            {
                                //if (b == '\r')
                                //    break;
                                *c1++ = b;
                                // check if something went wrong
                                if (counter >= 254)
                                {
                                    fclose(ps_fp);
                                    goto start_over;
                                }
                            }
                            *c1 = 0;

                        }
                    }
                    
					// clean up for new data
					{
						for (i = 0; i < old_num_chans; ++i) {
							// free(thread_info[i].index_array);
							// fclose(thread_info[i].d_fp);
                            if (!strcmp(f_name_temp[i],thread_info[i].f_name))
                            {
                                temp_channel_array[i] = thread_info[i].channel;
                            }
                            else
                            {
                                if (thread_info[i].channel->number_of_segments > 0)
                                    thread_info[i].channel->segments[0].metadata_fps->directives.free_password_data = MEF_TRUE;
                                free_channel(thread_info[i].channel, MEF_TRUE);
                                temp_channel_array[i] = NULL;
                            }
						}
                        if (fixed_info.page_data != NULL)
						    free(fixed_info.page_data);
                        if (thread_info != NULL)
						    free(thread_info);
#ifndef _WIN32
						free(thread_ids);
#endif
						rewind(o_fp);
						if (DBUG) printf("rewind\n");
						fixed_info.curr_view_sec = first_sec_written = curr_view_sec;
						//last_sec_written = first_sec_written - secs_per_page;
					}

					// allocate new threads
					{
						thread_info = (THREAD_INFO *) calloc((size_t) num_chans, sizeof(THREAD_INFO));
#ifndef _WIN32
						thread_ids = (pthread_t *) calloc((size_t) num_chans, sizeof(pthread_t));
#endif
						for (i = 0; i < num_chans; ++i) {
							thread_info[i].chan_idx = i;
							thread_info[i].fixed_info = &fixed_info;
						}
					}
		
					// open_files
                    // TBD make it threaded to speed it up
					{
						for (i = 0; i < num_chans; ++i) {
							sprintf(temp_path, "%s%s", data_path, thread_info[i].f_name);
							//thread_info[i].d_fp = fopen(temp_path, "r");
                            strcpy(thread_info[i].f_name, f_name_temp[i]);
                            if (temp_channel_array[i] == NULL)
                            {
#ifndef _WIN32
                                pthread_create(thread_ids + i, NULL, get_mef_channel_thread, (void *) (thread_info + i));
#else
 								thread_ids[i] = CreateThread(NULL, 0, get_mef_channel_thread, (void*) (thread_info + i), 0, &ThreadId);

#endif
                            }
                            else
                            {
#ifndef _WIN32
                                pthread_create(thread_ids + i, NULL, do_nothing_thread, (void *) (thread_info + i));
#else
								thread_ids[i] = CreateThread(NULL, 0, do_nothing_thread, (void*)(thread_info + i), 0, &ThreadId);

#endif
                                thread_info[i].channel = temp_channel_array[i];
                            }
						}
                        for (i=0;i<num_chans;i++)
                        {
#ifndef _WIN32
                            pthread_join(thread_ids[i], &ret_val);
#else
 							WaitForSingleObject(thread_ids[i], INFINITE);
#endif
                            fprintf(stderr, "%s\n", thread_info[i].f_name);
                            fprintf(stderr, "Segments in file: %d\n", thread_info[i].channel->number_of_segments);
                        }
                        
					}
                    
                    if (password_needed)
                    {
                        char command[1024];
                        sprintf(command, "touch %s", password_needed_path);
                        system(command);
                        exit(1);
                    }
                    
                    // re-order channels based on channel num
                    {
                        THREAD_INFO temp_info;
                        int did_swap;
                        
                        while (1)
                        {
                            did_swap = 0;
                            for (i=0;i<num_chans-1;i++)
                            {
                                for (j=i+1;j<num_chans;j++)
                                {
                                    int do_swap = 0;
                                    if (thread_info[i].channel->metadata.time_series_section_2->acquisition_channel_number > thread_info[j].channel->metadata.time_series_section_2->acquisition_channel_number)
                                        do_swap = 1;
                                    
                                    if (do_swap == 1)
                                    {
                                        did_swap = 1;
                                        memcpy(&temp_info, &thread_info[i], sizeof(THREAD_INFO));
                                        memcpy(&thread_info[i], &thread_info[j], sizeof(THREAD_INFO));
                                        memcpy(&thread_info[j], &temp_info, sizeof(THREAD_INFO));
                                        thread_info[i].chan_idx = i;
                                        thread_info[j].chan_idx = j;
                                        
                                    }
                                }
                            }
                            if (did_swap == 0)
                                break;
                        }
                        
                        fixed_info.session_start_time = -1;
                        fixed_info.session_end_time = -1;
                        for (i=0;i<num_chans;i++)
                        {
                            if (fixed_info.session_start_time == -1)
                                fixed_info.session_start_time = thread_info[i].channel->earliest_start_time;
                            if (thread_info[i].channel->earliest_start_time < fixed_info.session_start_time)
                                fixed_info.session_start_time = thread_info[i].channel->earliest_start_time;
                            
                            if (fixed_info.session_end_time == -1)
                                fixed_info.session_end_time = thread_info[i].channel->latest_end_time;
                            if (thread_info[i].channel->latest_end_time > fixed_info.session_end_time)
                                fixed_info.session_end_time = thread_info[i].channel->latest_end_time;
                        }
                        
                        if (curr_view_sec == 0)
                            curr_view_sec = fixed_info.curr_view_sec = fixed_info.session_start_time / 1000000.0;
                      
                        
                        // update server_info file
#ifndef _WIN32
                        while ((si_fp = fopen(server_info_path, "w")) == NULL) usleep((useconds_t) 100000);
#else
                        while ((si_fp = fopen(server_info_path, "wb")) == NULL) Sleep(100);
#endif
                        fprintf(si_fp, "%d\n", num_chans);
                        for (i=0;i<num_chans;i++)
                        {
                            fprintf(si_fp, "%s ", thread_info[i].f_name);
#ifndef _WIN32
                            fprintf(si_fp, "%ld %ld ", thread_info[i].channel->earliest_start_time, thread_info[i].channel->latest_end_time);
#else
                            fprintf(si_fp, "%lld %lld ", thread_info[i].channel->earliest_start_time, thread_info[i].channel->latest_end_time);
#endif
#ifndef _WIN32
                            fprintf(si_fp, "%ld ", thread_info[i].channel->metadata.time_series_section_2->acquisition_channel_number);
#else
                            fprintf(si_fp, "%lld ", thread_info[i].channel->metadata.time_series_section_2->acquisition_channel_number);
#endif
                            fprintf(si_fp, "%f\n", thread_info[i].channel->metadata.time_series_section_2->units_conversion_factor);
                        }
                        fprintf(si_fp, "%d\n", num_chans);
                        fclose(si_fp);
                    }
                    
                    
					// read samples / secs per page (per channel)
					{
						fscanf(ps_fp, "%d\n", &samps_per_page);
						fscanf(ps_fp, "%lf\n", &secs_per_page);

						fixed_info.samps_per_page = samps_per_page;
						fixed_info.secs_per_page = secs_per_page;
						tot_samps_per_page = num_chans * samps_per_page;
						fixed_info.page_data = (sf4 *) calloc((size_t) tot_samps_per_page, sizeof(sf4));
						last_sec_written = first_sec_written - secs_per_page;
						fscanf(ps_fp, "%s\n", password);
						if (DBUG) printf("pwd %s\n", password);
                        fscanf(ps_fp, "%s\n", events_file);

						if (DBUG) printf("Last sec written %lf\n", last_sec_written);
					}
                    
                    
                    // read events files, if they exist
                    {
                        si1        name[MEF_BASE_FILE_NAME_BYTES];
                        si1        events_ridx[1024];
                        si1        events_rdat[1024];
                        si8        number_of_records;
                        FILE_PROCESSING_STRUCT *rdat_fps;
                        RECORD_HEADER    *record_header;
                        PASSWORD_DATA    *pwd;
                        FILE    *events_out;
                        ui4   type_code;
                        ui4  *type_string_int;
                        ui1  *ui1_p;
                        MEFREC_Epoc_1_0* epoc_ptr;
                        
                        events_out = NULL;
                        
                        // look for case where file not specified, ie. default .rdat/.rdix pair at session level
                        if (!strcmp(events_file, "blank"))
                        {
                            extract_path_parts(data_path, NULL, name, NULL);
                            sprintf(events_ridx, "%s/%s.ridx", data_path, name);
                            sprintf(events_rdat, "%s/%s.rdat", data_path, name);
                            
                            fprintf(stderr, "PATHS: %s, %s\n", events_ridx, events_rdat);
                            
                            rdat_fps = read_MEF_file(NULL, events_rdat, fixed_info.password, NULL, NULL, USE_GLOBAL_BEHAVIOR);
                            
                            // if default session rdat file exists, then read it.
                            // TODO: look for default .rdat files within channels and segments
                            if (rdat_fps != NULL)
                            {
                                
                                fprintf(stderr, "READ RDAT FILE\n");
                                
                                number_of_records = rdat_fps->universal_header->number_of_entries;
                                ui1_p = rdat_fps->raw_data + UNIVERSAL_HEADER_BYTES;
                                pwd = rdat_fps->password_data;
                                
                                for (i = 0; i < number_of_records; ++i)
                                {
                                    record_header = (RECORD_HEADER *) ui1_p;
                                    
                                    type_string_int = (ui4 *) record_header->type_string;
                                    type_code = *type_string_int;
                                    
                                    if (events_out == NULL)
                                    {
                                        events_out = fopen(events_path, "w");
                                    }
                                    
                                    // TODO: read other event types
                                    switch (type_code){
                                        case MEFREC_Note_TYPE_CODE:
#ifndef _WIN32
                                            fprintf(events_out, "%ld,%s,%s\n", record_header->time, "Note", (si1*) record_header + MEFREC_Note_1_0_TEXT_OFFSET);
#else
                                            fprintf(events_out, "%lld,%s,%s\n", record_header->time, "Note", (si1*)record_header + MEFREC_Note_1_0_TEXT_OFFSET);
#endif
                                            break;
                                        case MEFREC_Epoc_TYPE_CODE:
                                            epoc_ptr = (MEFREC_Epoc_1_0*)((si1*)record_header + RECORD_HEADER_BYTES);
#ifndef _WIN32
                                            fprintf(events_out,"%ld,%s,%ld,%s,%s\n", record_header->time, "Epoch", epoc_ptr->duration, (si1*)record_header + MEFREC_Epoc_1_0_TYPE_OFFSET, (si1*)record_header + MEFREC_Epoc_1_0_TEXT_OFFSET);
#else
                                            fprintf(events_out,"%lld,%s,%lld,%s,%s\n", record_header->time, "Epoch", epoc_ptr->duration, (si1*)record_header + MEFREC_Epoc_1_0_TYPE_OFFSET, (si1*)record_header + MEFREC_Epoc_1_0_TEXT_OFFSET);
#endif
                                            break;
                                    }
                                    
                                    //show_record(record_header, i, pwd);
                                    ui1_p += (RECORD_HEADER_BYTES + record_header->bytes);
                                }
                                
                                fclose(events_out);
                                
                            }  // found events .rdat file
                        }
                        
                        // TODO: look for other specified event files
                    }
                    
                    // create major-discontinuity file
                    // this looks for major gaps in the data, and sends them to the UI so that info can
                    // be presented to the user
                    //
                    // Use first channel to be representative of the whole session
                    {
                        CHANNEL *channel;
                        si8 n_segments;
                        int i,j;
                        si8 end_of_previous_block;
                        si8 block_start_time;
                        FILE    *discon_out;
                        
                        channel = thread_info[0].channel;
                        end_of_previous_block = -1;
                        n_segments = channel->number_of_segments;
                        discon_out = fopen(discon_path, "w");
                        
                        for (i = 0; i < n_segments; ++i)
                        {
                            for (j = 0; j < channel->segments[i].metadata_fps->metadata.time_series_section_2->number_of_blocks; j++)
                            {
                                block_start_time = channel->segments[i].time_series_indices_fps->time_series_indices[j].start_time;
                                remove_recording_time_offset( &block_start_time);
                                
                                if (end_of_previous_block != -1)
                                {
                                    if (block_start_time - end_of_previous_block >= DISCON_MAJOR_THRESHOLD)
                                    {
                                        // write out to file
#ifndef _WIN32
                                        fprintf(discon_out, "%ld,%ld\n", end_of_previous_block, block_start_time);
#else
                                        fprintf(discon_out, "%lld,%lld\n", end_of_previous_block, block_start_time);
#endif
                                    }
                                }
                                
                                end_of_previous_block = block_start_time +
                                    (channel->segments[i].time_series_indices_fps->time_series_indices[j].number_of_samples *
                                     (1000000.0 / channel->metadata.time_series_section_2->sampling_frequency));
                            }
                        }
                            
                        fclose(discon_out);
                    }
                    
                    // set thread-specific variables
                    {
                        for (i = 0; i < num_chans; ++i)
                        {
                            thread_info[i].native_fs = thread_info[i].channel->metadata.time_series_section_2->sampling_frequency;
                        }
                    }
                    
                }
                //printf("fclose\n");
                fclose(ps_fp);
                read_files_flag = 0;
            }
        }
        
        // if N_PAGES_AHEAD number of pages have been buffered, then we're done reading (for now).
        // However, read_files_flag is still be set to 1 periodically, via the timer.
        if ((last_sec_written - curr_view_sec) >= (N_PAGES_AHEAD * secs_per_page)) {
#ifndef _WIN32
            usleep((useconds_t)250000);
#else
			Sleep(250);
#endif
            continue;
        }

        // thread out the reads (page wise)
        if (DBUG) printf("thread out reads\n");
        fixed_info.page_to_write_start_sec = last_sec_written + secs_per_page;
        
        for (i = 0; i < num_chans; ++i) {
            //			printf("create thread %d\n", i);
#ifndef _WIN32
            pthread_create(thread_ids + i, NULL, read_thread, (void*)(thread_info + i));
#else
			thread_ids[i] = CreateThread(NULL, 0, read_thread, (void*)(thread_info + i), 0, &ThreadId);
#endif
        }
        // wait for threads
        for (i = 0; i < num_chans; ++i) {
            //if (DBUG) printf("join thread %d\n", i);
#ifndef _WIN32
            pthread_join(thread_ids[i], &ret_val);
#else
			WaitForSingleObject(thread_ids[i], INFINITE);
#endif
        }
        //		printf("fwrite page_data\n");
        fwrite(fixed_info.page_data, sizeof(sf4), (size_t)tot_samps_per_page, o_fp);

        // On each iteration of the endless loop, we're adding a single page (of all requested channels) to the output file.
        last_sec_written += secs_per_page;

        // update buffer limits file
        if (check_fud(ps_path, nfud))
            read_files_flag = 1;
        else
            last_heartbeat = update_buffer_limits(buff_lim_path, first_sec_written, last_sec_written);

    } // end infinite loop

    // clean up for quit
    for (i = 0; i < num_chans; ++i) {
        //free(thread_info[i].index_array);
        //fclose(thread_info[i].d_fp);
        if (thread_info[i].channel->number_of_segments > 0)
            thread_info[i].channel->segments[0].metadata_fps->directives.free_password_data = MEF_TRUE;
        free_channel(thread_info[i].channel, MEF_TRUE);
    }
    free(fixed_info.page_data);
    free(thread_info);
#ifndef _WIN32
    free(thread_ids);
#endif
    fclose(o_fp);

    return(0);
}

#ifndef _WIN32
static void* get_mef_channel_thread(void* argument)
#else
DWORD WINAPI get_mef_channel_thread(LPVOID argument)
#endif
{
    THREAD_INFO* thread_info;
    char command[1024];

    //access passed argument
    thread_info = (THREAD_INFO*)argument;

    thread_info->channel = read_MEF_channel(NULL, thread_info->f_name, TIME_SERIES_CHANNEL_TYPE, thread_info->fixed_info->password, NULL, MEF_FALSE, MEF_FALSE);

    {
        CHANNEL* channel;
        int i;
        int j;

        channel = thread_info->channel;
   
        // verify password for first segment
        if (channel->number_of_segments > 0)
        {
            // check if channel needs decryption
            //if ((all_zeros(channel->segments[0].metadata_fps->universal_header->level_1_password_validation_field, PASSWORD_VALIDATION_FIELD_BYTES) == MEF_FALSE) ||
            //    (all_zeros(channel->segments[0].metadata_fps->universal_header->level_2_password_validation_field, PASSWORD_VALIDATION_FIELD_BYTES) == MEF_FALSE))
            if  ((channel->segments[0].metadata_fps->metadata.section_1->section_2_encryption != NO_ENCRYPTION) ||
                 (channel->segments[0].metadata_fps->metadata.section_1->section_3_encryption != NO_ENCRYPTION))
            {
                // level 1 access needed to read techinal data (level 2 not really needed)
                if (channel->segments[0].metadata_fps->password_data->access_level < LEVEL_1_ACCESS)
                    password_needed = 1;
            }
        }
        // fix bad data
        for (i = 1; i < channel->number_of_segments; i++)
        {
            if (channel->segments[i].metadata_fps->metadata.time_series_section_2->number_of_blocks > 0)
            {
                if (channel->segments[i].time_series_indices_fps->time_series_indices[0].start_sample ==
                    channel->segments[i].metadata_fps->metadata.time_series_section_2->start_sample)
                    
                {
                    for (j = 0; j < channel->segments[i].metadata_fps->metadata.time_series_section_2->number_of_blocks; j++)
                    {
                        channel->segments[i].time_series_indices_fps->time_series_indices[j].start_sample -= channel->segments[i].metadata_fps->metadata.time_series_section_2->start_sample;

                    }
                }
            }
        }

    }
    
    return(NULL);
}

#ifndef _WIN32
static void *do_nothing_thread(void *argument)
#else
DWORD WINAPI do_nothing_thread(LPVOID argument)
#endif
{
    return(NULL);
}

// "read_thread" reads one page of one channel
#ifndef _WIN32
static void *read_thread(void *argument)
#else
DWORD WINAPI read_thread(LPVOID argument)
#endif
{
    si4		i, j, chan_idx, cd_len, current_val, last_val, num_chans, samps_per_page;
    si4		*diff_buffer, *data, *dp, *dbp, offset_to_start_samp, offset_to_end_samp;
    THREAD_INFO	*thread_info;
    FIXED_INFO	*fixed_info;
    si8     start_time, end_time;
    ui8		*samp_idxs, start_samp, end_samp, num_samps, data_len, bytesDecoded, entryCounter;
    sf4		*page_data;
    sf8		out_samp_period, next_samp, curr_samp;
    ui4 n_segments;
    sf8 native_samp_freq;
    CHANNEL    *channel;
    si4 start_segment, end_segment;
    si4 times_specified, samples_specified;
    si8  total_samps, samp_counter_base;
    ui8  total_data_bytes;
    ui8  total_bytes_read;
    ui8 start_idx, end_idx, num_blocks;
    si4 *raw_data_buffer, *idp;
    si1 *compressed_data_buffer, *cdp;
    si8  segment_start_sample, segment_end_sample;
    si8  segment_start_time, segment_end_time;
    si8  block_start_time, block_end_time;
    si4 num_block_in_segment;
    si4 pass_order, stop_order;
    si4 order;
    FILE *fp;
    ui8 n_read, bytes_to_read;
    RED_PROCESSING_STRUCT	*rps;
    sf8 *filt_data;
    sf8         *pass_filt_nums, *pass_filt_dens;
    sf8         *stop_filt_nums, *stop_filt_dens;
    si4 sample_counter;
    ui4			max_samps;
    si4 offset_into_output_buffer;
    si8 block_start_time_offset;
    si4 *temp_data_buf;
    
    pass_order = 0;
    stop_order = 0;
    
    
    //access passed argument
    thread_info = (THREAD_INFO *) argument;
    fixed_info = thread_info->fixed_info;
    chan_idx = thread_info->chan_idx;
    samps_per_page = fixed_info->samps_per_page;
    page_data = fixed_info->page_data;
    num_chans = fixed_info->num_chans;
	
    
    start_time = fixed_info->page_to_write_start_sec * 1000000;
    end_time = (fixed_info->page_to_write_start_sec + fixed_info->secs_per_page) * 1000000;
#ifndef _WIN32
    if (DBUG) printf("start %ld end %ld\n", start_time, end_time);
#else
    if (DBUG) printf("start %lld end %lld\n", start_time, end_time);
#endif
    
    // MEF 3
    times_specified = 1;
    native_samp_freq = thread_info->native_fs;
    channel = thread_info->channel;
    
    if (times_specified)
        num_samps = (ui4)((((end_time - start_time) / 1000000.0) * channel->metadata.time_series_section_2->sampling_frequency) + 0.5);
    
    // Iterate through segments, looking for data that matches our criteria
    n_segments = channel->number_of_segments;
    start_segment = end_segment = -1;
    
    if (times_specified) {
        start_samp = sample_for_uutc_c(start_time, channel);
        end_samp = sample_for_uutc_c(end_time, channel);
        samples_specified = 1;
    }
    
    start_segment = 0;
    
    // Iterate through segments and see which segments the start and end are in.
    if (samples_specified) {
        for (i = 0; i < n_segments; ++i) {
            
            segment_start_sample = channel->segments[i].metadata_fps->metadata.time_series_section_2->start_sample;
            segment_end_sample   = channel->segments[i].metadata_fps->metadata.time_series_section_2->start_sample +
            channel->segments[i].metadata_fps->metadata.time_series_section_2->number_of_samples;
            
            if ((start_samp >= segment_start_sample) &&
                (start_samp <= segment_end_sample))
                start_segment = i;
            if ((end_samp >= segment_start_sample) &&
                (end_samp <= segment_end_sample))
                end_segment = i;
        }
    }
    
    if (end_segment == -1)
        end_segment = n_segments - 1;
    
    // TBD this should be re-done to include a partial page if partial data is available.
    // This code should never occur, since start_segment will always be >= 0, and end_segment will
    // always be < n_segments
    if ((start_segment == -1) || (end_segment == -1)) //hit the end of the file, fill out data with zeros
    {
        for (j=0; j < samps_per_page; )
        page_data[(j++ * num_chans) + chan_idx]=0;
        
        return(NULL);
    }
    
    //fprintf(stderr, "start seg = %d, end seg = %d\n", start_segment, end_segment);
    
    // find start block in start segment
    samp_counter_base = channel->segments[start_segment].metadata_fps->metadata.time_series_section_2->start_sample;
    for (j = 1; j < channel->segments[start_segment].metadata_fps->metadata.time_series_section_2->number_of_blocks; j++) {
        
        block_start_time = channel->segments[start_segment].time_series_indices_fps->time_series_indices[j].start_time;
        remove_recording_time_offset( &block_start_time);
        
        if ((samples_specified) &&
            (channel->segments[start_segment].time_series_indices_fps->time_series_indices[j].start_sample + samp_counter_base > start_samp)) {
            start_idx = j - 1;
            break;
        }
        // starting point is in last block in segment
        start_idx = j;
    }
    
    // find stop block in stop segment
    samp_counter_base = channel->segments[end_segment].metadata_fps->metadata.time_series_section_2->start_sample;
    for (j = 1; j < channel->segments[end_segment].metadata_fps->metadata.time_series_section_2->number_of_blocks; j++) {
        
        block_start_time = channel->segments[end_segment].time_series_indices_fps->time_series_indices[j].start_time;
        remove_recording_time_offset( &block_start_time);
        
        if ((samples_specified) &&
            (channel->segments[end_segment].time_series_indices_fps->time_series_indices[j].start_sample + samp_counter_base > end_samp)) {
            end_idx = j - 1;
            break;
        }
        // ending point is in last block in segment
        end_idx = j;
    }
    
    if (DBUG) fprintf(stderr, "start_segment = %d end_segment = %d\n", start_segment, end_segment);
    if (DBUG) fprintf(stderr, "start_idx = %d end_idx = %d\n", start_idx, end_idx);
    if (DBUG) fprintf(stderr, "start_samp = %d end_samp = %d\n", start_samp, end_samp);
    
    // find total_samps and total_data_bytes, so we can allocate buffers
    total_samps = 0;
    total_data_bytes = 0;
    
    // TBD do we care about this?
    //chan->max_sample_value = RED_MINIMUM_SAMPLE_VALUE;
    //chan->min_sample_value = RED_MAXIMUM_SAMPLE_VALUE;
    
    // normal case - everything is in one segment
    if (start_segment == end_segment) {
        if (end_idx < (channel->segments[start_segment].metadata_fps->metadata.time_series_section_2->number_of_blocks - 1)) {
            total_samps += channel->segments[start_segment].time_series_indices_fps->time_series_indices[end_idx+1].start_sample -
            channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].start_sample;
            //fprintf(stderr, "total_samps = %d\n", total_samps);
            total_data_bytes += channel->segments[start_segment].time_series_indices_fps->time_series_indices[end_idx+1].file_offset -
            channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].file_offset;
        }
        else {
            // case where end_idx is last block in segment
            total_samps += channel->segments[start_segment].metadata_fps->metadata.time_series_section_2->number_of_samples -
            channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].start_sample;
            total_data_bytes += channel->segments[start_segment].time_series_data_fps->file_length -
            channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].file_offset;
        }
        num_blocks = end_idx - start_idx + 1;
    }
    // spans across segments
    else {
        // start with first segment
        num_block_in_segment = channel->segments[start_segment].metadata_fps->metadata.time_series_section_2->number_of_blocks;
        total_samps += channel->segments[start_segment].metadata_fps->metadata.time_series_section_2->number_of_samples -
        channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].start_sample;
        total_data_bytes +=  channel->segments[start_segment].time_series_data_fps->file_length -
        channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].file_offset;
        num_blocks = num_block_in_segment - start_idx;
        
        // this loop will only run if there are segments in between the start and stop segments
        for (i = (start_segment + 1); i <= (end_segment - 1); i++) {
            num_block_in_segment = channel->segments[i].metadata_fps->metadata.time_series_section_2->number_of_blocks;
            total_samps += channel->segments[i].metadata_fps->metadata.time_series_section_2->number_of_samples;
            total_data_bytes += channel->segments[i].time_series_data_fps->file_length -
            channel->segments[i].time_series_indices_fps->time_series_indices[0].file_offset;
            num_blocks += num_block_in_segment;
        }
        
        // then last segment
        num_block_in_segment = channel->segments[end_segment].metadata_fps->metadata.time_series_section_2->number_of_blocks;
        if (end_idx < (channel->segments[end_segment].metadata_fps->metadata.time_series_section_2->number_of_blocks - 1)) {
            total_samps += channel->segments[end_segment].time_series_indices_fps->time_series_indices[end_idx+1].start_sample -
            channel->segments[end_segment].time_series_indices_fps->time_series_indices[0].start_sample;
            total_data_bytes += channel->segments[end_segment].time_series_indices_fps->time_series_indices[end_idx+1].file_offset -
            channel->segments[end_segment].time_series_indices_fps->time_series_indices[0].file_offset;
            num_blocks += end_idx + 1;
        }
        else {
            // case where end_idx is last block in segment
            total_samps += channel->segments[end_segment].metadata_fps->metadata.time_series_section_2->number_of_samples -
            channel->segments[end_segment].time_series_indices_fps->time_series_indices[0].start_sample;
            total_data_bytes += channel->segments[end_segment].time_series_data_fps->file_length -
            channel->segments[end_segment].time_series_indices_fps->time_series_indices[0].file_offset;
            num_blocks += end_idx + 1;
        }
    }
    
    // allocate buffers
    data_len = total_samps;
    order = (pass_order > stop_order) ? pass_order : stop_order;
    //filt_data = (sf8 *) calloc((size_t) data_len + (6 * order), sizeof(sf8));
    compressed_data_buffer = (si1 *) malloc((size_t) total_data_bytes + 30);
    // TBD fix undiagnosed abort problem
    // where the RED_Decode runs off the end of the cdp buffer - this occurs even though
    // CRC checks are passing on the block, and the stated size of the block is correct
    // Can't be a RED_Decode problem since it only  happens at the last block on the right
    // side of the screen - if it was a block/RED issue, then it would happen randomly
    // (Unless ... it overruns the buffer on every block, and just crashses on the right-most one...)
    // Will fix this later once I have time
    // For now just adding 30 bytes to buffer to prevent buffer overrun abort
    raw_data_buffer = (si4 *) malloc((size_t) (num_samps * sizeof(si4)));
    memset_int(raw_data_buffer, RED_NAN, num_samps);
    cdp = compressed_data_buffer;
    idp = raw_data_buffer;
    total_bytes_read = 0;  // this only matters when reading across segment boundaries
    
    // read in RED data
    // normal case - everything is in one segment
    if (start_segment == end_segment) {
        fp = channel->segments[start_segment].time_series_data_fps->fp;
        fseek(fp, channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].file_offset, SEEK_SET);
        n_read = fread(cdp, sizeof(si1), (size_t) total_data_bytes, fp);
    }
    // spans across segments
    else {
        // start with first segment
        fp = channel->segments[start_segment].time_series_data_fps->fp;
        fseek(fp, channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].file_offset, SEEK_SET);
        bytes_to_read = channel->segments[start_segment].time_series_data_fps->file_length -
        channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].file_offset;
        n_read = fread(cdp, sizeof(si1), (size_t) bytes_to_read, fp);
        cdp += bytes_to_read;
        total_bytes_read += bytes_to_read;
        
        // this loop will only run if there are segments in between the start and stop segments
        for (i = (start_segment + 1); i <= (end_segment - 1); i++) {
            fp = channel->segments[i].time_series_data_fps->fp;
            fseek(fp, UNIVERSAL_HEADER_BYTES, SEEK_SET);
            bytes_to_read = channel->segments[i].time_series_data_fps->file_length -
                channel->segments[i].time_series_indices_fps->time_series_indices[0].file_offset;
            n_read = fread(cdp, sizeof(si1), (size_t) bytes_to_read, fp);
            cdp += bytes_to_read;
            total_bytes_read += bytes_to_read;
        }
        
        // then last segment
        fp = channel->segments[end_segment].time_series_data_fps->fp;
        fseek(fp, UNIVERSAL_HEADER_BYTES, SEEK_SET);
        //bytes_to_read = channel->segments[end_segment].time_series_data_fps->file_length -
        //channel->segments[end_segment].time_series_indices_fps->time_series_indices[start_idx].file_offset;
        bytes_to_read = total_data_bytes - total_bytes_read;
        n_read = fread(cdp, sizeof(si1), (size_t) bytes_to_read, fp);
        cdp += bytes_to_read;
    }
    
    // set up RED processing struct
    cdp = compressed_data_buffer;
    max_samps = channel->metadata.time_series_section_2->maximum_block_samples;
    
    // create RED processing struct
    rps = (RED_PROCESSING_STRUCT *) calloc((size_t) 1, sizeof(RED_PROCESSING_STRUCT));
    rps->compression.mode = RED_DECOMPRESSION;
    //rps->directives.return_block_extrema = MEF_TRUE;
    rps->decompressed_ptr = rps->decompressed_data = idp;
    rps->difference_buffer = (si1 *) e_calloc((size_t) RED_MAX_DIFFERENCE_BYTES(max_samps), sizeof(ui1), __FUNCTION__, __LINE__, USE_GLOBAL_BEHAVIOR);
    
    //thread_info->number_of_discontinuities = 0;
    sample_counter = 0;
    
    temp_data_buf = NULL;
    
    // decode first block to temp array
    if (num_blocks >= 1)
    {
        temp_data_buf = (int *) malloc((max_samps * 1.1) * sizeof(si4));
        rps->decompressed_ptr = rps->decompressed_data = temp_data_buf;
        rps->compressed_data = cdp;
        rps->block_header = (RED_BLOCK_HEADER *) rps->compressed_data;
        
        if (!check_block_crc((ui1*)(rps->block_header), max_samps, compressed_data_buffer, total_data_bytes))
        {
            fprintf(stdout, "**CRC block failure!**\n");
            goto skip_rest_of_decoding;
        }
        
        RED_decode(rps);
        cdp += rps->block_header->block_bytes;
        
        if (times_specified)
        {
            // rps->block_header->start_time is already offset during RED_decode()
            
            if ((rps->block_header->start_time - start_time) >= 0)
                offset_into_output_buffer = (si4) ((((rps->block_header->start_time - start_time) / 1000000.0) * channel->metadata.time_series_section_2->sampling_frequency) + 0.5);
            else
                offset_into_output_buffer = (si4) ((((rps->block_header->start_time - start_time) / 1000000.0) * channel->metadata.time_series_section_2->sampling_frequency) - 0.5);
        }
        else
            offset_into_output_buffer = (si4) (channel->segments[start_segment].metadata_fps->metadata.time_series_section_2->start_sample +
                                                          channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].start_sample) - start_samp;
        
        // copy requested samples from first block to output buffer
        // TBD this loop could be optimized
        for (i=0;i<rps->block_header->number_of_samples;i++)
        {
            if (offset_into_output_buffer < 0)
            {
                offset_into_output_buffer++;
                continue;
            }
            
            if ((ui4) offset_into_output_buffer >= num_samps)
                break;
            
            *(raw_data_buffer + offset_into_output_buffer) = temp_data_buf[i];
            
            offset_into_output_buffer++;
        }
        sample_counter = offset_into_output_buffer;
        
    }
    
    // decode bytes to samples
    for (i=1;i<num_blocks-1;i++) {
        rps->compressed_data = cdp;
        rps->block_header = (RED_BLOCK_HEADER *) rps->compressed_data;
        if (!check_block_crc((ui1*)(rps->block_header), max_samps, compressed_data_buffer, total_data_bytes))
        {
            fprintf(stdout, "*****************CRC block failure!**\n");
            goto skip_rest_of_decoding;
        }
        
        if (times_specified)
        {
            block_start_time_offset = rps->block_header->start_time;
            remove_recording_time_offset( &block_start_time_offset );
            
            // The next two checks see if the block contains out-of-bounds samples.
            // In that case, skip the block and move on
            if (block_start_time_offset < start_time)
            {
                cdp += rps->block_header->block_bytes;
                continue;
            }
            if (block_start_time_offset + ((rps->block_header->number_of_samples / channel->metadata.time_series_section_2->sampling_frequency) * 1e6) >= end_time)
            {
                // Comment this out for now, it creates a strange boundary condition
                // cdp += rps->block_header->block_bytes;
                continue;
            }
            
            rps->decompressed_ptr = rps->decompressed_data = raw_data_buffer + (int)((((block_start_time_offset - start_time) / 1000000.0) * channel->metadata.time_series_section_2->sampling_frequency) + 0.5);
        }
        
        
        RED_decode(rps);

        cdp += rps->block_header->block_bytes;
        idp += rps->block_header->number_of_samples;
        rps->decompressed_ptr = rps->decompressed_data = idp;
        
        sample_counter += rps->block_header->number_of_samples;
    }
    
    // decode last block to temp array
    if (num_blocks >= 2)
    {
        // temp_data_buf already is malloc'd by this point
        rps->decompressed_ptr = rps->decompressed_data = temp_data_buf;
        rps->compressed_data = cdp;
        rps->block_header = (RED_BLOCK_HEADER *) rps->compressed_data;
        
        
        if (!check_block_crc((ui1*)(rps->block_header), max_samps, compressed_data_buffer, total_data_bytes))
        {
            fprintf(stdout, "**CRC block failure!**\n");
            goto skip_rest_of_decoding;
        }
        
        RED_decode(rps);
        cdp += rps->block_header->block_bytes;
        
        if (times_specified)
        {
            if ((rps->block_header->start_time - start_time) >= 0)
                offset_into_output_buffer = (si4) ((((rps->block_header->start_time - start_time) / 1000000.0) * channel->metadata.time_series_section_2->sampling_frequency) + 0.5);
            else
                offset_into_output_buffer = (si4) ((((rps->block_header->start_time - start_time) / 1000000.0) * channel->metadata.time_series_section_2->sampling_frequency) - 0.5);
        }
        else
            offset_into_output_buffer = (si4) channel->segments[start_segment].time_series_indices_fps->time_series_indices[start_idx].start_sample - start_samp;
        
        // copy requested samples from first block to output buffer
        // TBD this loop could be optimized
        for (i=0;i<rps->block_header->number_of_samples;i++)
        {
            if (offset_into_output_buffer < 0)
            {
                offset_into_output_buffer++;
                continue;
            }
            
            if ((ui4) offset_into_output_buffer >= num_samps)
                break;
            
            *(raw_data_buffer + offset_into_output_buffer) = temp_data_buf[i];
            
            offset_into_output_buffer++;
        }
        sample_counter = offset_into_output_buffer;
        
    }
    
skip_rest_of_decoding:
    
    // we're done with the compressed data, get rid of it
    if (compressed_data_buffer != NULL)
        free (compressed_data_buffer);
    if (rps != NULL)
    {
        if (rps->difference_buffer != NULL)
            free(rps->difference_buffer);
        free(rps);
    }
    
    // TBD add filtering back in?  More difficult now since data may contain NaNs.
    // for now do filtering the same filtering we were doing before
    
    // pass filter
    //    if (pass_order){
    //        filtfilt(pass_filt_nums, pass_filt_dens, pass_order, raw_data_buffer, data_len, filt_data, NULL, NULL);
    //        for (i = 0; i < data_len; ++i)
    //            raw_data_buffer[i] = (si4) (filt_data[i] + 0.5);
    //    }
    //
    //    // stop filter
    //    if (stop_order) {
    //        filtfilt(stop_filt_nums, stop_filt_dens, stop_order, raw_data_buffer, data_len, filt_data, NULL, NULL);
    //        for (i = 0; i < data_len; ++i)
    //            raw_data_buffer[i] = (si4) (filt_data[i] + 0.5);
    //    }
    //
    
    if (times_specified)
        out_samp_period = (sf8) (((end_time - start_time) / 1000000.0) * channel->metadata.time_series_section_2->sampling_frequency) / (sf8) fixed_info->samps_per_page;
    else
        out_samp_period = (sf8) (end_samp - start_samp) / (sf8) fixed_info->samps_per_page;
    
    if (DBUG) printf("out_samp_period  %lf samps_per_page %d\n", out_samp_period, samps_per_page);
    
    dp = raw_data_buffer;
    next_samp = 0;
    curr_samp = 0;
    last_val = *dp;
    
    //interpolate and copy to page_data
    i=0;
    if (DBUG) printf("interpolating and copying to pg data: next samp: %d, curr samp: %d\n", next_samp, curr_samp);
    
    //page_data[chan_idx] = 0.0;  // set first samp to zero (will probably get overwritten)
    for (j=0;j<samps_per_page;j++)
        page_data[(j * num_chans) + chan_idx] = (sf4) NAN;  // set all rest to NAN
    
    for (j=0; j < samps_per_page; ) {
        current_val = *dp++;
        if (curr_samp >= next_samp) {
            if (current_val == RED_NAN || last_val == RED_NAN)
                page_data[(j * num_chans) + chan_idx] = (sf4) NAN;  // python (numpy) recognizes this NAN value
            else
                page_data[(j * num_chans) + chan_idx] = (sf4) (((curr_samp - next_samp) * (current_val - last_val)) + last_val) * channel->metadata.time_series_section_2->units_conversion_factor;
            next_samp += out_samp_period;
            j++;
        }
        last_val = current_val;
        curr_samp += 1.0;
        
        // check for over-running buffer
        // this actually happens - is the logic bad for this loop?
        // logic looks okay - but sometimes at the right-most page of a recording, it runs way past the end of raw_data_buffer.
        // The way next_samp is initially caluclated might be incorrect in some cases.  If this becomes a big problem,
        // it should be debugged more fully.
        // Seems fixed now?
        if (i >= num_samps)
        {
            if (DBUG) fprintf(stdout, "**** BUFFER OVERFLOW detected when downsampling %d  *****\n", j);
                break;
        }
        
        i++;
    }
    
    if (raw_data_buffer != NULL)
        free(raw_data_buffer);
    if (temp_data_buf != NULL)
        free(temp_data_buf);
    
    return(NULL);
}


#ifndef _WIN32
static void set_rf_flag(int x)
{
	read_files_flag = 1;
	
	return;
}
#else
void CALLBACK set_rf_flag(HWND hwnd, UINT uMsg, UINT timerId, DWORD dwTime)
{
    while (1)
    {
        read_files_flag = 1;

        //printf("RF TIMER\n");

        Sleep(500);
    }

    return;
}
#endif


static ui8 update_buffer_limits(char* buff_lim_path, double first_sec_written, double last_sec_written)
{
	FILE *bl_fp;
	ui8 curr_time;
	
	curr_time = time(NULL);
	if (DBUG) printf("updating buffer limits\n");
	
	// update buffer limits file
#ifndef _WIN32
	while ((bl_fp = fopen(buff_lim_path, "w")) == NULL) usleep((useconds_t) 100000);
#else
	while ((bl_fp = fopen(buff_lim_path, "wb")) == NULL) Sleep(100);
#endif
	fprintf(bl_fp, "%0.12lf\n%0.12lf\n", first_sec_written, last_sec_written);
#ifndef _WIN32
	fprintf(bl_fp, "%ld\n", curr_time); //add page server "heartbeat"
#else
    fprintf(bl_fp, "%lld\n", curr_time);
#endif
	fclose(bl_fp);
	
	return(curr_time);
	
}

static si4 check_fud(char *path, sf8 num) 
{
    FILE *fp;
    sf8 newnum;
    si4 out_val;
    
    
    if (DBUG) printf("in checkfud\n");
#ifndef _WIN32
    while ((fp = fopen(path, "r")) == NULL) usleep((useconds_t) 100000); 
#else
    while ((fp = fopen(path, "rb")) == NULL) Sleep(100); 
#endif
    if (DBUG) printf("wait done\n");

    fscanf(fp, "%lf\n", &newnum); 
    fclose(fp);
    if (DBUG) printf("file closed\n");
    
    if (newnum == num) 
        return(0);
    else
        return(1);

}

#ifndef _WIN32
static void *heartbeat_thread(void *argument)
{
    char *page_dir;
    char file_name[4096];
    FILE *fp;
    sf8 ui_time;
    
    page_dir = (THREAD_INFO *) argument;
    fprintf(stderr, "page_dir == %s", page_dir);
    
    sprintf(file_name, "%sHEARTBEAT_UI", page_dir);

    
    while (1)
    {
        
        while ((fp = fopen(file_name, "r")) == NULL) usleep((useconds_t) 100000); //page_specs file
        fscanf(fp, "%lf\n", &ui_time);
        fclose(fp);
        
        if (time(NULL) - ui_time > 5)
            exit(0);
        
        usleep((useconds_t) 500000);
        
    }
}
#else
DWORD WINAPI heartbeat_thread(LPVOID argument)
{
    char* page_dir;
    char file_name[4096];
    FILE* fp;
    sf8 ui_time;

    page_dir = (char*)argument;
    fprintf(stderr, "page_dir == %s", page_dir);

    sprintf(file_name, "%sHEARTBEAT_UI", page_dir);

    while (1)
    {

        while ((fp = fopen(file_name, "rb")) == NULL) Sleep(100); //page_specs file
        fscanf(fp, "%lf\n", &ui_time);
        fclose(fp);

        if (time(NULL) - ui_time > 5)
            exit(0);
        
        Sleep(500);

    }
}
#endif


si8 sample_for_uutc_c(si8 uutc, CHANNEL *channel)
{
    ui8 i, j, sample;
    sf8 native_samp_freq;
    ui8 prev_sample_number;
    si8 prev_time, seg_start_sample;
    si8 next_sample_number;
    
    native_samp_freq = channel->metadata.time_series_section_2->sampling_frequency;
    prev_sample_number = channel->segments[0].metadata_fps->metadata.time_series_section_2->start_sample;
    prev_time = channel->segments[0].time_series_indices_fps->time_series_indices[0].start_time;
    
    for (j = 0; j < (ui8) channel->number_of_segments; j++)
    {
        seg_start_sample = channel->segments[j].metadata_fps->metadata.time_series_section_2->start_sample;
        
        // initialize next_sample_number to end of current segment, in case we're on the last segment and we
        // go all the way to the end of the segment.
        // Otherwise this value will get overridden later on
        next_sample_number = seg_start_sample + channel->segments[j].metadata_fps->metadata.time_series_section_2->number_of_samples;
        
        for (i = 0; i < (ui8) channel->segments[j].metadata_fps->metadata.time_series_section_2->number_of_blocks; ++i) {
            if (channel->segments[j].time_series_indices_fps->time_series_indices[i].start_time > uutc)
            {
                next_sample_number = channel->segments[j].time_series_indices_fps->time_series_indices[i].start_sample + seg_start_sample;
                goto done;
            }
            prev_sample_number = channel->segments[j].time_series_indices_fps->time_series_indices[i].start_sample + seg_start_sample;
            prev_time = channel->segments[j].time_series_indices_fps->time_series_indices[i].start_time;
        }
    }
    
done:
    
    sample = prev_sample_number + (ui8) (((((sf8) (uutc - prev_time)) / 1000000.0) * native_samp_freq) + 0.5);
    if (sample > next_sample_number)
        sample = next_sample_number;  // prevent it from going too far
    
    return(sample);
}
