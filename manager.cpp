#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <string>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <netdb.h>
#include <resolv.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <pcap.h>
#include <libssh2.h>

#include <sstream>

#include "ipaccount.h"
#include "voipmonitor.h"
#include "calltable.h"
#include "sniff.h"
#include "format_slinear.h"
#include "codec_alaw.h"
#include "codec_ulaw.h"
#include "tools.h"
#include "calltable.h"
#include "format_ogg.h"
#include "cleanspool.h"
#include "pcap_queue.h"
#include "manager.h"
#include "fraud.h"

#define BUFSIZE 1024

extern Calltable *calltable;
extern int opt_manager_port;
extern char opt_manager_ip[32];
extern volatile int calls_counter;
extern char opt_clientmanager[1024];
extern int opt_clientmanagerport;
extern char mac[32];
extern int verbosity;
extern char opt_chdir[1024];
extern char opt_php_path[1024];
extern int terminating;
extern int manager_socket_server;
extern int terminating;
extern int opt_nocdr;
extern int global_livesniffer;
extern int global_livesniffer_all;
extern map<unsigned int, octects_live_t*> ipacc_live;

extern map<unsigned int, livesnifferfilter_t*> usersniffer;

extern char ssh_host[1024];
extern int ssh_port;
extern char ssh_username[256];
extern char ssh_password[256];
extern char ssh_remote_listenhost[1024];
extern unsigned int ssh_remote_listenport;

using namespace std;

struct listening_worker_arg {
	Call *call;
};

static void updateLivesnifferfilters();
static bool cmpCallBy_destroy_call_at(Call* a, Call* b);
static bool cmpCallBy_first_packet_time(Call* a, Call* b);

livesnifferfilter_use_siptypes_s livesnifferfilterUseSipTypes;

ManagerClientThreads ClientThreads;

/* 
 * this function runs as thread. It reads RTP audio data from call
 * and write it to output buffer 
 *
 * input parameter is structure where call 
 *
*/
void *listening_worker(void *arguments) {
	struct listening_worker_arg *args = (struct listening_worker_arg*)arguments;

        int ret = 0;
        unsigned char read1[1024];
        unsigned char read2[1024];
        struct timeval tv;
        int diff;

	getUpdDifTime(&tv);
	alaw_init();
	ulaw_init();

        struct timeval tvwait;

	short int r1;
	short int r2;
	int len1,len2;

	// if call is hanged hup it will set listening_worker_run in its destructor to 0
	int listening_worker_run = 1;
	args->call->listening_worker_run = &listening_worker_run;
	pthread_mutex_lock(&args->call->listening_worker_run_lock);


//	FILE *out = fopen("/tmp/test.raw", "w");
//	FILE *outa = fopen("/tmp/test.alaw", "w");

//	vorbis_desc ogg;
//	ogg_header(out, &ogg);
//	fclose(out);
//	pthread_mutex_lock(&args->call->buflock);
//	ogg_header_live(&args->call->spybufferchar, &ogg);
//	pthread_mutex_unlock(&args->call->buflock);

	timespec tS;
	timespec tS2;

	tS.tv_sec = 0;
	tS.tv_nsec = 0;
	tS2.tv_sec = 0;
	tS2.tv_nsec = 0;

	long int udiff;

        while(listening_worker_run) {

		if(tS.tv_nsec > tS2.tv_nsec) {
			udiff = (1000 * 1000 * 1000 - (tS.tv_nsec - tS2.tv_nsec)) / 1000;
		} else {
			udiff = (tS2.tv_nsec - tS.tv_nsec) / 1000;
		}

		tvwait.tv_sec = 0;
		tvwait.tv_usec = 1000*20 - udiff; //20 ms
//		long int usec = tvwait.tv_usec;
		ret = select(0, NULL, NULL, NULL, &tvwait);

		clock_gettime(CLOCK_REALTIME, &tS);
		char *s16char;

		//usleep(tvwait.tv_usec);
		pthread_mutex_lock(&args->call->buflock);
		diff = getUpdDifTime(&tv) / 1000;
		len1 = circbuf_read(args->call->audiobuffer1, (char*)read1, 160);
		len2 = circbuf_read(args->call->audiobuffer2, (char*)read2, 160);
//		printf("codec_caller[%d] codec_called[%d] len1[%d] len2[%d] outbc[%d] outbchar[%d] wait[%u]\n", args->call->codec_caller, args->call->codec_called, len1, len2, (int)args->call->spybuffer.size(), (int)args->call->spybufferchar.size(), usec);
		if(len1 == 160 and len2 == 160) {
			for(int i = 0; i < len1; i++) {
				switch(args->call->codec_caller) {
				case 0:
					r1 = ULAW(read1[i]);
					break;
				case 8:
					r1 = ALAW(read1[i]);
					break;
				}
					
				switch(args->call->codec_caller) {
				case 0:
					r2 = ULAW(read2[i]);
					break;
				case 8:
					r2 = ALAW(read2[i]);
					break;
				}
				s16char = (char *)&r1;
				slinear_saturated_add((short int*)&r1, (short int*)&r2);
				//fwrite(&r1, 1, 2, out);
				args->call->spybufferchar.push(s16char[0]);
				args->call->spybufferchar.push(s16char[1]);
//				ogg_write_live(&ogg, &args->call->spybufferchar, (short int*)&r1);
			}
		} else if(len2 == 160) {
			for(int i = 0; i < len2; i++) {
				switch(args->call->codec_caller) {
				case 0:
					r2 = ULAW(read2[i]);
					break;
				case 8:
					r2 = ALAW(read2[i]);
					break;
				}
				//fwrite(&r2, 1, 2, out);
				s16char = (char *)&r2;
				args->call->spybufferchar.push(s16char[0]);
				args->call->spybufferchar.push(s16char[1]);
//				ogg_write_live(&ogg, &args->call->spybufferchar, (short int*)&r2);
			}
		} else if(len1 == 160) {
			for(int i = 0; i < len1; i++) {
				switch(args->call->codec_caller) {
				case 0:
					r1 = ULAW(read1[i]);
					break;
				case 8:
					r1 = ALAW(read1[i]);
					break;
				}
				//fwrite(&r1, 1, 2, out);
				s16char = (char *)&r1;
				args->call->spybufferchar.push(s16char[0]);
				args->call->spybufferchar.push(s16char[1]);
//				ogg_write_live(&ogg, &args->call->spybufferchar, (short int*)&r1);
			}
		} else {
                        //printf("diff [%d] timeout\n", diff);
			// write 20ms silence 
			int16_t s = 0;
			//unsigned char sa = 255;
			for(int i = 0; i < 160; i++) {
				//fwrite(&s, 1, 2, out);
				s16char = (char *)&s;
				args->call->spybufferchar.push(s16char[0]);
				args->call->spybufferchar.push(s16char[1]);
//				ogg_write_live(&ogg, &args->call->spybufferchar, (short int*)&s);
			}
		}
		pthread_mutex_unlock(&args->call->buflock);
		clock_gettime(CLOCK_REALTIME, &tS2);
        }

	// reset pointer to NULL as we are leaving the stack here
	args->call->listening_worker_run = NULL;
	pthread_mutex_unlock(&args->call->listening_worker_run_lock);

	//clean ogg
/*
        ogg_stream_clear(&ogg.os);
        vorbis_block_clear(&ogg.vb);
        vorbis_dsp_clear(&ogg.vd);
        vorbis_comment_clear(&ogg.vc);
        vorbis_info_clear(&ogg.vi);
*/

	free(args);
	return 0;
}

int sendssh(LIBSSH2_CHANNEL *channel, const char *buf, int len) {
	int wr, i;
	wr = 0;
	do {   
		i = libssh2_channel_write(channel, buf, len);
		if (i < 0) {
			fprintf(stderr, "libssh2_channel_write: %d\n", i);
			return -1;
		}
		wr += i;
	} while(i > 0 && wr < len);
	return wr;
}

int sendvm(int socket, LIBSSH2_CHANNEL *channel, const char *buf, size_t len, int mode) {
	int res;
	if(channel) {
		res = sendssh(channel, buf, len);
	} else {
		res = send(socket, buf, len, 0);
	}
	return res;
}

int parse_command(char *buf, int size, int client, int eof, const char *buf_long, ManagerClientThread **managerClientThread = NULL, LIBSSH2_CHANNEL *sshchannel = NULL) {
	char sendbuf[BUFSIZE];
	u_int32_t uid = 0;

	if(strstr(buf, "getversion") != NULL) {
		if ((size = sendvm(client, sshchannel, RTPSENSOR_VERSION, strlen(RTPSENSOR_VERSION), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "reindexfiles") != NULL) {
		snprintf(sendbuf, BUFSIZE, "starting reindexing plesae wait...");
		if ((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		convert_filesindex();
		snprintf(sendbuf, BUFSIZE, "done\r\n");
		if ((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "totalcalls") != NULL) {
		snprintf(sendbuf, BUFSIZE, "%d", calls_counter);
		if ((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "disablecdr") != NULL) {
		opt_nocdr = 1;
		if ((size = sendvm(client, sshchannel, "disabled", 8, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "enablecdr") != NULL) {
		opt_nocdr = 0;
		if ((size = sendvm(client, sshchannel, "enabled", 7, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "listcalls") != NULL) {
		//list<Call*>::iterator call;
		map<string, Call*>::iterator callMAPIT;
		Call *call;
		char outbuf[2048];
		char *resbuf = (char*)realloc(NULL, 32 * 1024 * sizeof(char));;
		unsigned int resbufalloc = 32 * 1024, outbuflen = 0, resbuflen = 0;
		if(outbuf == NULL) {
			syslog(LOG_ERR, "Cannot allocate memory\n");
			return -1;
		}
		/* headers */
		outbuflen = sprintf(outbuf, 
				    "[[\"callreference\", "
				    "\"callid\", "
				    "\"callercodec\", "
				    "\"calledcodec\", "
				    "\"caller\", "
				    "\"callername\", "
				    "\"callerdomain\", "
				    "\"called\", "
				    "\"calleddomain\", "
				    "\"calldate\", "
				    "\"duration\", "
				    "\"connect_duration\", "
				    "\"callerip\", "
				    "\"calledip\", "
				    "\"lastpackettime\", "
				    "\"lastSIPresponseNum\"]");
		memcpy(resbuf + resbuflen, outbuf, outbuflen);
		resbuflen += outbuflen;
		calltable->lock_calls_listMAP();
		for (callMAPIT = calltable->calls_listMAP.begin(); callMAPIT != calltable->calls_listMAP.end(); ++callMAPIT) {
			call = (*callMAPIT).second;
			if(call->type == REGISTER or call->destroy_call_at > 0) {
				// skip register and calls which are scheduled to be closed
				continue;
			}
			/* 
			 * caller 
			 * callername
			 * called
			 * calldate
			 * duration
			 * callerip htonl(sipcallerip)
			 * sipcalledip htonl(sipcalledip)
			*/
			//XXX: escape " or replace it to '
			outbuflen = sprintf(outbuf, 
					    ",[\"%p\", "
					    "\"%s\", "
					    "\"%d\", "
					    "\"%d\", "
					    "\"%s\", "
					    "\"%s\", "
					    "\"%s\", "
					    "\"%s\", "
					    "\"%s\", "
					    "\"%d\", "
					    "\"%d\", "
					    "\"%d\", "
					    "\"%u\", "
					    "\"%u\", "
					    "\"%u\", "
					    "\"%d\"]",
					    call, 
					    call->call_id.c_str(), 
					    call->last_callercodec, 
					    call->last_callercodec, 
					    call->caller, 
					    call->callername, 
					    call->caller_domain,
					    call->called, 
					    call->called_domain,
					    call->calltime(), 
					    call->duration(), 
					    call->connect_duration(), 
					    htonl(call->sipcallerip), 
					    htonl(call->sipcalledip), 
					    (unsigned int)call->get_last_packet_time(), 
					    call->lastSIPresponseNum);
			if((resbuflen + outbuflen) > resbufalloc) {
				resbuf = (char*)realloc(resbuf, resbufalloc + 32 * 1024 * sizeof(char));
				resbufalloc += 32 * 1024;
			}
			memcpy(resbuf + resbuflen, outbuf, outbuflen);
			resbuflen += outbuflen;
		}
		calltable->unlock_calls_listMAP();
		if((resbuflen + 1) > resbufalloc) {
			resbuf = (char*)realloc(resbuf, resbufalloc + 32 * 1024 * sizeof(char));
			resbufalloc += 32 * 1024;
		}
		resbuf[resbuflen] = ']';
		resbuflen++;
		if ((size = sendvm(client, sshchannel, resbuf, resbuflen, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		free(resbuf);
		return 0;
	} else if(strstr(buf, "d_lc_for_destroy") != NULL) {
		ostringstream outStr;
		if(calltable->calls_queue.size()) {
			Call *call;
			vector<Call*> vectCall;
			calltable->lock_calls_queue();
			for(size_t i = 0; i < calltable->calls_queue.size(); ++i) {
				call = calltable->calls_queue[i];
				if(call->type != REGISTER && call->destroy_call_at) {
					vectCall.push_back(call);
				}
			}
			if(vectCall.size()) { 
				std::sort(vectCall.begin(), vectCall.end(), cmpCallBy_destroy_call_at);
				for(size_t i = 0; i < vectCall.size(); i++) {
					call = vectCall[i];
					outStr.width(15);
					outStr << call->caller << " -> ";
					outStr.width(15);
					outStr << call->called << "  "
					<< sqlDateTimeString(call->calltime()) << "  ";
					outStr.width(6);
					outStr << call->duration() << "s  "
					<< sqlDateTimeString(call->destroy_call_at) << "  "
					<< call->fbasename;
					outStr << endl;
				}
			}
			calltable->unlock_calls_queue();
		}
		outStr << "-----------" << endl;
		if ((size = sendvm(client, sshchannel, outStr.str().c_str(), outStr.str().length(), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "d_lc_bye") != NULL) {
		ostringstream outStr;
		map<string, Call*>::iterator callMAPIT;
		Call *call;
		vector<Call*> vectCall;
		calltable->lock_calls_listMAP();
		for (callMAPIT = calltable->calls_listMAP.begin(); callMAPIT != calltable->calls_listMAP.end(); ++callMAPIT) {
			call = (*callMAPIT).second;
			if(call->type != REGISTER && call->seenbye) {
				vectCall.push_back(call);
			}
		}
		if(vectCall.size()) { 
			std::sort(vectCall.begin(), vectCall.end(), cmpCallBy_destroy_call_at);
			for(size_t i = 0; i < vectCall.size(); i++) {
				call = vectCall[i];
				outStr.width(15);
				outStr << call->caller << " -> ";
				outStr.width(15);
				outStr << call->called << "  "
				<< sqlDateTimeString(call->calltime()) << "  ";
				outStr.width(6);
				outStr << call->duration() << "s  "
				<< (call->destroy_call_at ? sqlDateTimeString(call->destroy_call_at) : "    -  -     :  :  ")  << "  "
				<< call->fbasename;
				outStr << endl;
			}
		}
		calltable->unlock_calls_listMAP();
		outStr << "-----------" << endl;
		if ((size = sendvm(client, sshchannel, outStr.str().c_str(), outStr.str().length(), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "d_lc_all") != NULL) {
		ostringstream outStr;
		map<string, Call*>::iterator callMAPIT;
		Call *call;
		vector<Call*> vectCall;
		calltable->lock_calls_listMAP();
		for (callMAPIT = calltable->calls_listMAP.begin(); callMAPIT != calltable->calls_listMAP.end(); ++callMAPIT) {
			vectCall.push_back((*callMAPIT).second);
		}
		if(vectCall.size()) { 
			std::sort(vectCall.begin(), vectCall.end(), cmpCallBy_first_packet_time);
			for(size_t i = 0; i < vectCall.size(); i++) {
				call = vectCall[i];
				outStr.width(15);
				outStr << call->caller << " -> ";
				outStr.width(15);
				outStr << call->called << "  "
				<< sqlDateTimeString(call->calltime()) << "  ";
				outStr.width(6);
				outStr << call->duration() << "s  "
				<< (call->destroy_call_at ? sqlDateTimeString(call->destroy_call_at) : "    -  -     :  :  ")  << "  ";
				outStr.width(3);
				outStr << call->lastSIPresponseNum << "  "
				<< call->fbasename;
				outStr << endl;
			}
		}
		calltable->unlock_calls_listMAP();
		outStr << "-----------" << endl;
		if ((size = sendvm(client, sshchannel, outStr.str().c_str(), outStr.str().length(), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "d_close_call") != NULL) {
		char fbasename[100];
		sscanf(buf, "d_close_call %s", fbasename);
		string rslt = fbasename + string(" missing");
		map<string, Call*>::iterator callMAPIT;
		calltable->lock_calls_listMAP();
		for (callMAPIT = calltable->calls_listMAP.begin(); callMAPIT != calltable->calls_listMAP.end(); ++callMAPIT) {
			if(!strcmp((*callMAPIT).second->fbasename, fbasename)) {
				(*callMAPIT).second->force_close = true;
				rslt = fbasename + string(" close");
				break;
			}
		}
		calltable->unlock_calls_listMAP();
		if ((size = sendvm(client, sshchannel, (rslt + "\n").c_str(), rslt.length() + 1, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "getipaccount") != NULL) {
		sscanf(buf, "getipaccount %u", &uid);
		map<unsigned int, octects_live_t*>::iterator it = ipacc_live.find(uid);
		if(it != ipacc_live.end()) {
			snprintf(sendbuf, BUFSIZE, "%d", 1);
		} else {
			snprintf(sendbuf, BUFSIZE, "%d", 0);
		}
		if ((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "ipaccountfilter set") != NULL) {
		
		string ipfilter;
		if(buf_long) {
			buf = (char*)buf_long;
		}
		u_int32_t id = atol(buf + strlen("ipaccountfilter set "));
		char *pointToSeparatorBefereIpfilter = strchr(buf + strlen("ipaccountfilter set "), ' ');
		if(pointToSeparatorBefereIpfilter) {
			ipfilter = pointToSeparatorBefereIpfilter + 1;
		}
		if(!ipfilter.length() || ipfilter.find("ALL") != string::npos) {
			map<unsigned int, octects_live_t*>::iterator it = ipacc_live.find(id);
			octects_live_t* filter;
			if(it != ipacc_live.end()) {
				filter = it->second;
			} else {
				filter = (octects_live_t*)calloc(1, sizeof(octects_live_t));
				filter->all = 1;
				filter->fetch_timestamp = time(NULL);
				ipacc_live[id] = filter;
				if(verbosity > 0) {
					cout << "START LIVE IPACC " << "id: " << id << " ipfilter: " << "ALL" << endl;
				}
			}
			return 0;
		} else {
			map<unsigned int, octects_live_t*>::iterator ipacc_liveIT = ipacc_live.find(id);
			octects_live_t* filter;
			filter = (octects_live_t*)calloc(1, sizeof(octects_live_t));
			filter->setFilter(ipfilter.c_str());
			filter->fetch_timestamp = time(NULL);
			ipacc_live[id] = filter;
			if(verbosity > 0) {
				cout << "START LIVE IPACC " << "id: " << id << " ipfilter: " << ipfilter << endl;
			}
		}
		return(0);
	} else if(strstr(buf, "stopipaccount")) {
		u_int32_t id = 0;
		sscanf(buf, "stopipaccount %u", &id);
		map<unsigned int, octects_live_t*>::iterator it = ipacc_live.find(id);
		if(it != ipacc_live.end()) {
			free(it->second);
			ipacc_live.erase(it);
			if(verbosity > 0) {
				cout << "STOP LIVE IPACC " << "id:" << id << endl;
			}
		}
		return 0;
	} else if(strstr(buf, "fetchipaccount")) {
		u_int32_t id = 0;
		sscanf(buf, "fetchipaccount %u", &id);
		map<unsigned int, octects_live_t*>::iterator it = ipacc_live.find(id);
		char sendbuf[1024];
		if(it == ipacc_live.end()) {
			strcpy(sendbuf, "stopped");
		} else {
			octects_live_t *data = it->second;
			snprintf(sendbuf, 1024, "%u;%llu;%u;%llu;%u;%llu;%u;%llu;%u;%llu;%u;%llu;%u", 
				(unsigned int)time(NULL),
				data->dst_octects, data->dst_numpackets, 
				data->src_octects, data->src_numpackets, 
				data->voipdst_octects, data->voipdst_numpackets, 
				data->voipsrc_octects, data->voipsrc_numpackets, 
				data->all_octects, data->all_numpackets,
				data->voipall_octects, data->voipall_numpackets);
			data->fetch_timestamp = time(NULL);
		}
		if((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1) {
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
///////////////////////////////////////////////////////////////
        } else if(strstr(buf, "stoplivesniffer")) {
                sscanf(buf, "stoplivesniffer %u", &uid);
                map<unsigned int, livesnifferfilter_t*>::iterator usersnifferIT = usersniffer.find(uid);
                if(usersnifferIT != usersniffer.end()) {
                        free(usersnifferIT->second);
                        usersniffer.erase(usersnifferIT);
			if(!usersniffer.size()) {
				global_livesniffer = 0;
				//global_livesniffer_all = 0;
			}
			updateLivesnifferfilters();
			if(verbosity > 0) {
				 syslog(LOG_NOTICE, "stop livesniffer - uid: %u", uid);
			}
                }
                return 0;
	} else if(strstr(buf, "getlivesniffer") != NULL) {
		sscanf(buf, "getlivesniffer %u", &uid);
		map<unsigned int, livesnifferfilter_t*>::iterator usersnifferIT = usersniffer.find(uid);
		if(usersnifferIT != usersniffer.end()) {
			snprintf(sendbuf, BUFSIZE, "%d", 1);
		} else {
			snprintf(sendbuf, BUFSIZE, "%d", 0);
		}
		if ((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "livefilter set") != NULL) {
		char search[1024] = "";
		char value[1024] = "";

		global_livesniffer_all = 0;
		sscanf(buf, "livefilter set %u %s %[^\n\r]", &uid, search, value);
		if(verbosity > 0) {
			syslog(LOG_NOTICE, "set livesniffer - uid: %u search: %s value: %s", uid, search, value);
		}

		if(memmem(search, sizeof(search), "all", 3)) {
			global_livesniffer = 1;
			map<unsigned int, livesnifferfilter_t*>::iterator usersnifferIT = usersniffer.find(uid);
			livesnifferfilter_t* filter;
			if(usersnifferIT != usersniffer.end()) {
				filter = usersnifferIT->second;
			} else {
				filter = (livesnifferfilter_t*)calloc(1, sizeof(livesnifferfilter_t));
				usersniffer[uid] = filter;
			}
			updateLivesnifferfilters();
			return 0;
		}

		map<unsigned int, livesnifferfilter_t*>::iterator usersnifferIT = usersniffer.find(uid);
		livesnifferfilter_t* filter;
		if(usersnifferIT != usersniffer.end()) {
			filter = usersnifferIT->second;
		} else {
			filter = (livesnifferfilter_t*)calloc(1, sizeof(livesnifferfilter_t));
			usersniffer[uid] = filter;
		}

		if(strstr(search, "srcaddr")) {
			int i = 0;
			//reset filters 
			for(i = 0; i < MAXLIVEFILTERS; i++) {
				filter->lv_saddr[i] = 0;
			}
			stringstream  data(value);
			string val;
			// read all argumens lkivefilter set saddr 123 345 244
			i = 0;
			while(i < MAXLIVEFILTERS and getline(data, val,' ')){
				global_livesniffer = 1;
				//convert doted ip to unsigned int
				filter->lv_bothaddr[i] = ntohl((unsigned int)inet_addr(val.c_str()));
				i++;
			}
			updateLivesnifferfilters();
		} else if(strstr(search, "dstaddr")) {
			int i = 0;
			//reset filters 
			for(i = 0; i < MAXLIVEFILTERS; i++) {
				filter->lv_daddr[i] = 0;
			}
			stringstream  data(value);
			string val;
			i = 0;
			// read all argumens livefilter set daddr 123 345 244
			while(i < MAXLIVEFILTERS and getline(data, val,' ')){
				global_livesniffer = 1;
				//convert doted ip to unsigned int
				filter->lv_bothaddr[i] = ntohl((unsigned int)inet_addr(val.c_str()));
				i++;
			}
			updateLivesnifferfilters();
		} else if(strstr(search, "bothaddr")) {
			int i = 0;
			//reset filters 
			for(i = 0; i < MAXLIVEFILTERS; i++) {
				filter->lv_bothaddr[i] = 0;
			}
			stringstream  data(value);
			string val;
			i = 0;
			// read all argumens livefilter set bothaddr 123 345 244
			while(i < MAXLIVEFILTERS and getline(data, val,' ')){
				global_livesniffer = 1;
				//convert doted ip to unsigned int
				filter->lv_bothaddr[i] = ntohl((unsigned int)inet_addr(val.c_str()));
				i++;
			}
			updateLivesnifferfilters();
		} else if(strstr(search, "srcnum")) {
			int i = 0;
			//reset filters 
			for(i = 0; i < MAXLIVEFILTERS; i++) {
				filter->lv_srcnum[i][0] = '\0';
			}
			stringstream  data(value);
			string val;
			i = 0;
			// read all argumens livefilter set srcaddr 123 345 244
			while(i < MAXLIVEFILTERS and getline(data, val,' ')){
				global_livesniffer = 1;
				stringstream tmp;
				tmp << val;
				tmp >> filter->lv_srcnum[i];
				//cout << filter->lv_srcnum[i] << "\n";
				i++;
			}
			updateLivesnifferfilters();
		} else if(strstr(search, "dstnum")) {
			int i = 0;
			//reset filters 
			for(i = 0; i < MAXLIVEFILTERS; i++) {
				filter->lv_dstnum[i][0] = '\0';
			}
			stringstream  data(value);
			string val;
			i = 0;
			// read all argumens livefilter set dstaddr 123 345 244
			while(i < MAXLIVEFILTERS and getline(data, val,' ')){
				global_livesniffer = 1;
				stringstream tmp;
				tmp << val;
				tmp >> filter->lv_dstnum[i];
				//cout << filter->lv_dstnum[i] << "\n";
				i++;
			}
			updateLivesnifferfilters();
		} else if(strstr(search, "bothnum")) {
			int i = 0;
			//reset filters 
			for(i = 0; i < MAXLIVEFILTERS; i++) {
				filter->lv_bothnum[i][0] = '\0';
			}
			stringstream  data(value);
			string val;
			i = 0;
			// read all argumens livefilter set bothaddr 123 345 244
			while(i < MAXLIVEFILTERS and getline(data, val,' ')){
				global_livesniffer = 1;
				stringstream tmp;
				tmp << val;
				tmp >> filter->lv_bothnum[i];
				//cout << filter->lv_bothnum[i] << "\n";
				i++;
			}
			updateLivesnifferfilters();
		} else if(strstr(search, "siptypes")) {
			//cout << "siptypes: " << value << "\n";
			for(size_t i = 0; i < strlen(value) && i < MAXLIVEFILTERS; i++) {
				filter->lv_siptypes[i] = value[i] == 'I' ? INVITE :
							 value[i] == 'R' ? REGISTER :
							 value[i] == 'O' ? OPTIONS :
							 value[i] == 'S' ? SUBSCRIBE :
							 value[i] == 'M' ? MESSAGE :
									   0;
			}
			updateLivesnifferfilters();
		}
	} else if(strstr(buf, "listen") != NULL) {
		long long callreference;

		intptr_t tmp1,tmp2;

		sscanf(buf, "listen %llu", &callreference);

		tmp1 = callreference;
	
		map<string, Call*>::iterator callMAPIT;
		Call *call;
		calltable->lock_calls_listMAP();
		for (callMAPIT = calltable->calls_listMAP.begin(); callMAPIT != calltable->calls_listMAP.end(); ++callMAPIT) {
			call = (*callMAPIT).second;
			tmp2 = (intptr_t)call;

			//printf("call[%p] == [%li] [%d] [%li] [%li]\n", call, callreference, (long int)call == (long int)callreference, (long int)call, (long int)callreference);
				
			//if((long long)call == (long long)callreference) {
			if(tmp1 == tmp2) {
				if(call->listening_worker_run) {
					// the thread is already running. 
					if ((size = sendvm(client, sshchannel, "call already listening", 22, 0)) == -1){
						cerr << "Error sending data to client" << endl;
						return -1;
					}
					calltable->unlock_calls_listMAP();
					return 0;
				} else {
					struct listening_worker_arg *args = (struct listening_worker_arg*)malloc(sizeof(listening_worker_arg));
					args->call = call;
					call->audiobuffer1 = new pvt_circbuf;
					call->audiobuffer2 = new pvt_circbuf;
					circbuf_init(call->audiobuffer1, 4092);
					circbuf_init(call->audiobuffer2, 4092);

					pthread_t call_thread;
					pthread_create(&call_thread, NULL, listening_worker, (void *)args);
					calltable->unlock_calls_listMAP();
					if ((size = sendvm(client, sshchannel, "success", 7, 0)) == -1){
						cerr << "Error sending data to client" << endl;
						return -1;
					}
					return 0;
				}
			}
		}
		calltable->unlock_calls_listMAP();
		if ((size = sendvm(client, sshchannel, "call not found", 14, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "readaudio") != NULL) {
		long long callreference;

		sscanf(buf, "readaudio %llu", &callreference);
	
		map<string, Call*>::iterator callMAPIT;
		Call *call;
		int i;
		calltable->lock_calls_listMAP();
		for (callMAPIT = calltable->calls_listMAP.begin(); callMAPIT != calltable->calls_listMAP.end(); ++callMAPIT) {
			call = (*callMAPIT).second;
			if((long int)call == (long int)callreference) {
				pthread_mutex_lock(&call->buflock);
				size_t bsize = call->spybufferchar.size();
				char *buff = (char*)malloc(sizeof(char) * bsize);
				for(i = 0; i < (int)bsize; i++) {
					buff[i] = call->spybufferchar.front();
					call->spybufferchar.pop();
				}
				pthread_mutex_unlock(&call->buflock);
				if ((size = sendvm(client, sshchannel, buff, bsize, 0)) == -1){
					free(buff);
					calltable->unlock_calls_listMAP();
					cerr << "Error sending data to client" << endl;
					return -1;
				}
				free(buff);
			}
		}
		calltable->unlock_calls_listMAP();
		return 0;
	} else if(strstr(buf, "reload") != NULL) {
		reload_config();
		if ((size = sendvm(client, sshchannel, "reload ok", 9, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "fraud_refresh") != NULL) {
		refreshFraud();
		if ((size = sendvm(client, sshchannel, "reload ok", 9, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "getfile") != NULL) {
		char filename[2048];
		char rbuf[4096];
		int fd;
		ssize_t nread;

		sscanf(buf, "getfile %s", filename);

		fd = open(filename, O_RDONLY);
		if(fd < 0) {
			sprintf(buf, "error: cannot open file [%s]", filename);
			if ((size = sendvm(client, sshchannel, buf, strlen(buf), 0)) == -1){
				cerr << "Error sending data to client" << endl;
			}
			return -1;
		}
		while(nread = read(fd, rbuf, sizeof rbuf), nread > 0) {
			if ((size = sendvm(client, sshchannel, rbuf, nread, 0)) == -1){
				close(fd);
				return -1;
			}
		}
		close(fd);
		return 0;
	} else if(strstr(buf, "file_exists") != NULL) {
		char filename[2048];
		unsigned int size;
		char outbuf[100];

		sscanf(buf, "file_exists %s", filename);
		if(FileExists(filename)) {
			size = file_exists(filename);
			sprintf(outbuf, "%d", size);
		} else {
			strcpy(outbuf, "not_exists");
		}
		sendvm(client, sshchannel, outbuf, strlen(outbuf), 0);
		return 0;
	} else if(strstr(buf, "fileexists") != NULL) {
		char filename[2048];
		unsigned int size;

		sscanf(buf, "fileexists %s", filename);
		size = file_exists(filename);
		sprintf(buf, "%d", size);
		sendvm(client, sshchannel, buf, strlen(buf), 0);
		return 0;
	} else if(strstr(buf, "genwav") != NULL) {
		char filename[2048];
		unsigned int size;
		char wavfile[2048];
		char pcapfile[2048];
		char cmd[4092];
		int secondrun = 0;

		sscanf(buf, "genwav %s", filename);

		sprintf(pcapfile, "%s.pcap", filename);
		sprintf(wavfile, "%s.wav", filename);

getwav2:
		size = file_exists(wavfile);
		if(size) {
			sprintf(buf, "%d", size);
			sendvm(client, sshchannel, buf, strlen(buf), 0);
			return 0;
		}
		if(secondrun > 0) {
			// wav does not exist 
			sendvm(client, sshchannel, "0", 1, 0);
			return -1;
		}

		// wav does not exists, check if exists pcap and try to create wav
		size = file_exists(pcapfile);
		if(!size) {
			sendvm(client, sshchannel, "0", 1, 0);
			return -1;
		}
		sprintf(cmd, "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/sbin:/usr/local/bin voipmonitor --rtp-firstleg -k -WRc -r \"%s.pcap\" -y -d %s 2>/dev/null >/dev/null", filename, opt_chdir);
		system(cmd);
		secondrun = 1;
		goto getwav2;
	} else if(strstr(buf, "getwav") != NULL) {
		char filename[2048];
		int fd;
		unsigned int size;
		char wavfile[2048];
		char pcapfile[2048];
		char cmd[4092];
		char rbuf[4096];
		int res;
		ssize_t nread;
		int secondrun = 0;

		sscanf(buf, "getwav %s", filename);

		sprintf(pcapfile, "%s.pcap", filename);
		sprintf(wavfile, "%s.wav", filename);

getwav:
		size = file_exists(wavfile);
		if(size) {
			fd = open(wavfile, O_RDONLY);
			if(fd < 0) {
				sprintf(buf, "error: cannot open file [%s]", wavfile);
				if ((res = sendvm(client, sshchannel, buf, strlen(buf), 0)) == -1){
					cerr << "Error sending data to client" << endl;
				}
				return -1;
			}
			while(nread = read(fd, rbuf, sizeof rbuf), nread > 0) {
				if ((res = sendvm(client, sshchannel, rbuf, nread, 0)) == -1){
					close(fd);
					return -1;
				}
			}
			if(eof) {
				if ((res = sendvm(client, sshchannel, "EOF", 3, 0)) == -1){
					close(fd);
					return -1;
				}
			}
			close(fd);
			return 0;
		}
		if(secondrun > 0) {
			// wav does not exist 
			sendvm(client, sshchannel, "0", 1, 0);
			return -1;
		}

		// wav does not exists, check if exists pcap and try to create wav
		size = file_exists(pcapfile);
		if(!size) {
			sendvm(client, sshchannel, "0", 1, 0);
			return -1;
		}
		sprintf(cmd, "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/sbin:/usr/local/bin voipmonitor --rtp-firstleg -k -WRc -r \"%s.pcap\" -y 2>/dev/null >/dev/null", filename);
		system(cmd);
		secondrun = 1;
		goto getwav;
	} else if(strstr(buf, "getsiptshark") != NULL) {
		char filename[2048];
		int fd;
		unsigned int size;
		char tsharkfile[2048];
		char pcapfile[2048];
		char cmd[4092];
		char rbuf[4096];
		int res;
		ssize_t nread;

		sscanf(buf, "getsiptshark %s", filename);

		sprintf(tsharkfile, "%s.pcap2txt", filename);
		sprintf(pcapfile, "%s.pcap", filename);


		size = file_exists(tsharkfile);
		if(size) {
			fd = open(tsharkfile, O_RDONLY);
			if(fd < 0) {
				sprintf(buf, "error: cannot open file [%s]", tsharkfile);
				if ((res = sendvm(client, sshchannel, buf, strlen(buf), 0)) == -1){
					cerr << "Error sending data to client" << endl;
				}
				return -1;
			}
			while(nread = read(fd, rbuf, sizeof rbuf), nread > 0) {
				if ((res = sendvm(client, sshchannel, rbuf, nread, 0)) == -1){
					close(fd);
					return -1;
				}
			}
			if(eof) {
				if ((res = sendvm(client, sshchannel, "EOF", 3, 0)) == -1){
					close(fd);
					return -1;
				}
			}
			close(fd);
			return 0;
		}

		size = file_exists(pcapfile);
		if(!size) {
			sendvm(client, sshchannel, "0", 1, 0);
			return -1;
		}
	
		sprintf(cmd, "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin tshark -r \"%s.pcap\" -R sip > \"%s.pcap2txt\" 2>/dev/null", filename, filename);
		system(cmd);
		sprintf(cmd, "echo ==== >> \"%s.pcap2txt\"", filename);
		system(cmd);
		sprintf(cmd, "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin tshark -r \"%s.pcap\" -V -R sip >> \"%s.pcap2txt\" 2>/dev/null", filename, filename);
		system(cmd);

		size = file_exists(tsharkfile);
		if(size) {
			fd = open(tsharkfile, O_RDONLY);
			if(fd < 0) {
				sprintf(buf, "error: cannot open file [%s]", filename);
				return -1;
			}
			while(nread = read(fd, rbuf, sizeof rbuf), nread > 0) {
				if ((res = sendvm(client, sshchannel, rbuf, nread, 0)) == -1){
					close(fd);
					return -1;
				}
			}
			if(eof) {
				if ((res = sendvm(client, sshchannel, "EOF", 3, 0)) == -1){
					close(fd);
					return -1;
				}
			}
			close(fd);
			return 0;
		}
		return 0;
	} else if(strstr(buf, "quit") != NULL) {
		return 0;
	} else if(strstr(buf, "terminating") != NULL) {
		terminating = 1;
	} else if(strstr(buf, "coutstr") != NULL) {
		char *pointToSpaceSeparator = strchr(buf, ' ');
		if(pointToSpaceSeparator) {
			cout << (pointToSpaceSeparator + 1) << flush;
		}
	} else if(strstr(buf, "syslogstr") != NULL) {
		char *pointToSpaceSeparator = strchr(buf, ' ');
		if(pointToSpaceSeparator) {
			syslog(LOG_NOTICE, pointToSpaceSeparator + 1);
		}
	} else if(strstr(buf, "custipcache_get_cust_id") != NULL) {
		char ip[20];
		sscanf(buf, "custipcache_get_cust_id %s", ip);
		extern CustIpCache *custIpCache;
		unsigned int cust_id = custIpCache->getCustByIp(inet_addr(ip));
		snprintf(sendbuf, BUFSIZE, "cust_id: %u\n", cust_id);
		if((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1) {
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "custipcache_refresh") != NULL) {
		extern CustIpCache *custIpCache;
		custIpCache->clear();
		int rslt = custIpCache->fetchAllIpQueryFromDb();
		snprintf(sendbuf, BUFSIZE, "rslt: %i\n", rslt);
		if((size = sendvm(client, sshchannel, sendbuf, strlen(sendbuf), 0)) == -1) {
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "custipcache_vect_print") != NULL) {
		extern CustIpCache *custIpCache;
		string rslt = custIpCache->printVect();
		if((size = sendvm(client, sshchannel, rslt.c_str(), rslt.length(), 0)) == -1) {
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "restart") != NULL ||
		  strstr(buf, "upgrade") != NULL) {
		bool upgrade = false;
		string version;
		string url;
		string md5_32;
		string md5_64;
		string rsltForSend;
		if(strstr(buf, "upgrade") != NULL) {
			upgrade = true;
			string command = buf;
			size_t pos = command.find("to: [");
			if(pos != string::npos) {
				size_t posEnd = command.find("]", pos);
				if(posEnd != string::npos) {
					version = command.substr(pos + 5, posEnd - pos - 5);
				}
			}
			if(pos != string::npos) {
				pos = command.find("url: [", pos);
				if(pos != string::npos) {
					size_t posEnd = command.find("]", pos);
					if(posEnd != string::npos) {
						url = command.substr(pos + 6, posEnd - pos - 6);
					}
				}
			}
			if(pos != string::npos) {
				pos = command.find("md5: [", pos);
				if(pos != string::npos) {
					size_t posEnd = command.find("]", pos);
					if(posEnd != string::npos) {
						md5_32 = command.substr(pos + 6, posEnd - pos - 6);
					}
					pos = command.find(" / [", pos);
					if(pos != string::npos) {
						size_t posEnd = command.find("]", pos);
						if(posEnd != string::npos) {
							md5_64 = command.substr(pos + 4, posEnd - pos - 4);
						}
					}
				}
			}
			if(!version.length()) {
				rsltForSend = "missing version in command line";
			} else if(!url.length()) {
				rsltForSend = "missing url in command line";
			} else if(!md5_32.length() || !md5_64.length()) {
				rsltForSend = "missing md5 in command line";
			}
		}
		bool ok = false;
		RestartUpgrade restart(upgrade, version.c_str(), url.c_str(), md5_32.c_str(), md5_64.c_str());
		if(!rsltForSend.length()) {
			if(restart.createRestartScript()) {
				if((!upgrade || restart.runUpgrade()) &&
				   restart.checkReadyRestart() &&
				   restart.isOk()) {
					ok = true;
				}
			}
			rsltForSend = restart.getRsltString();
		}
		if ((size = sendvm(client, sshchannel, rsltForSend.c_str(), rsltForSend.length(), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		if(ok) {
			restart.runRestart(client, manager_socket_server);
		}
		return 0;
	} else if(strstr(buf, "sniffer_stat") != NULL) {
		extern vm_atomic<string> storingCdrLastWriteAt;
		extern vm_atomic<string> pbStatString;
		extern vm_atomic<u_long> pbCountPacketDrop;
		ostringstream outStrStat;
		outStrStat << "{"
			   << "\"version\": \"" << RTPSENSOR_VERSION << "\","
			   << "\"storingCdrLastWriteAt\": \"" << storingCdrLastWriteAt << "\","
			   << "\"pbStatString\": \"" << pbStatString << "\","
			   << "\"pbCountPacketDrop\": \"" << pbCountPacketDrop << "\","
			   << "\"uptime\": \"" << getUptime() << "\""
			   << "}";
		outStrStat << endl;
		string outStrStatStr = outStrStat.str();
		if ((size = sendvm(client, sshchannel, outStrStatStr.c_str(), outStrStatStr.length(), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return 0;
	} else if(strstr(buf, "pcapstat") != NULL) {
		extern PcapQueue *pcapQueueStatInterface;
		string rslt;
		if(pcapQueueStatInterface) {
			rslt = pcapQueueStatInterface->pcapDropCountStat();
			if(!rslt.length()) {
				rslt = "ok";
			}
		} else {
			rslt = "no PcapQueue mode";
		}
		if ((size = sendvm(client, sshchannel, rslt.c_str(), rslt.length(), 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
		return(0);
	} else if(strstr(buf, "login_screen_popup") != NULL) {
		*managerClientThread =  new ManagerClientThread_screen_popup(client, buf);
	} else if(strstr(buf, "ac_add_thread") != NULL) {
		extern AsyncClose asyncClose;
		asyncClose.addThread();
		if ((size = sendvm(client, sshchannel, "ok\n", 3, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else if(strstr(buf, "ac_remove_thread") != NULL) {
		extern AsyncClose asyncClose;
		asyncClose.removeThread();
		if ((size = sendvm(client, sshchannel, "ok\n", 3, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	} else {
		if ((size = sendvm(client, sshchannel, "command not found\n", 18, 0)) == -1){
			cerr << "Error sending data to client" << endl;
			return -1;
		}
	}
	return 1;
}


void *manager_client(void *dummy) {
	struct hostent* host;
	struct sockaddr_in addr;
	int res;
	int client = 0;
	char buf[BUFSIZE];
	char sendbuf[BUFSIZE];
	int size;
	

	while(1) {
		host = gethostbyname(opt_clientmanager);
		if (!host) { //Report lookup failure  
			syslog(LOG_ERR, "Cannot resolv: %s: host [%s] trying again...\n",  hstrerror(h_errno),  opt_clientmanager);  
			sleep(1);
			continue;  
		} 
		break;
	}
connect:
	client = socket(PF_INET, SOCK_STREAM, 0); /* create socket */
	memset(&addr, 0, sizeof(addr));    /* create & zero struct */
	addr.sin_family = AF_INET;    /* select internet protocol */
	addr.sin_port = htons(opt_clientmanagerport);         /* set the port # */
	addr.sin_addr.s_addr = *(long*)host->h_addr_list[0]; /* set the addr */
	syslog(LOG_NOTICE, "Connecting to manager server [%s]\n", inet_ntoa( *(struct in_addr *) host->h_addr_list[0]));
	while(1) {
		res = connect(client, (struct sockaddr *)&addr, sizeof(addr));         /* connect! */
		if(res == -1) {
			syslog(LOG_NOTICE, "Failed to connect to server [%s] error:[%s] trying again...\n", inet_ntoa( *(struct in_addr *) host->h_addr_list[0]), strerror(errno));
			sleep(1);
			continue;
		}
		break;
	}

	// send login
	snprintf(sendbuf, BUFSIZE, "login %s", mac);
	if ((size = send(client, sendbuf, strlen(sendbuf), 0)) == -1){
		perror("send()");
		sleep(1);
		goto connect;
	}

	// catch the reply
	size = recv(client, buf, BUFSIZE - 1, 0);
	buf[size] = '\0';

	while(1) {

		string buf_long;
		//cout << "New manager connect from: " << inet_ntoa((in_addr)clientInfo.sin_addr) << endl;
		size = recv(client, buf, BUFSIZE - 1, 0);
		if (size == -1 or size == 0) {
			//cerr << "Error in receiving data" << endl;
			perror("recv()");
			close(client);
			sleep(1);
			goto connect;
		}
		buf[size] = '\0';
//		if(verbosity > 0) syslog(LOG_NOTICE, "recv[%s]\n", buf);
		//res = parse_command(buf, size, client, 1, buf_long.c_str());
		res = parse_command(buf, size, client, 1, NULL);
	
#if 0	
		//cout << "New manager connect from: " << inet_ntoa((in_addr)clientInfo.sin_addr) << endl;
		size = recv(client, buf, BUFSIZE - 1, 0);
		if (size == -1 or size == 0) {
			//cerr << "Error in receiving data" << endl;
			perror("recv()");
			close(client);
			sleep(1);
			goto connect;
		} else {
			buf[size] = '\0';
			buf_long = buf;
			char buf_next[BUFSIZE];
			while((size = recv(client, buf_next, BUFSIZE - 1, 0)) > 0) {
				buf_next[size] = '\0';
				buf_long += buf_next;
			}
		}
		buf[size] = '\0';
		if(verbosity > 0) syslog(LOG_NOTICE, "recv[%s]\n", buf);
		res = parse_command(buf, size, client, 1, buf_long.c_str());
#endif
	}

	return 0;
}

void *manager_read_thread(void * arg) {

	char buf[BUFSIZE];
	string buf_long;
	int size;
	unsigned int    client;
	client = *(unsigned int *)arg;

	//cout << "New manager connect from: " << inet_ntoa((in_addr)clientInfo.sin_addr) << endl;
	if ((size = recv(client, buf, BUFSIZE - 1, 0)) == -1) {
		cerr << "Error in receiving data" << endl;
		close(client);
		return 0;
	} else {
		buf[size] = '\0';
		buf_long = buf;
		////cout << "DATA: " << buf << endl;
		if(size == BUFSIZE - 1 && !strstr(buf, "\r\n\r\n")) {
			char buf_next[BUFSIZE];
			////cout << "NEXT_RECV start" << endl;
			while((size = recv(client, buf_next, BUFSIZE - 1, 0)) > 0) {
				buf_next[size] = '\0';
				buf_long += buf_next;
				////cout << "NEXT DATA: " << buf_next << endl;
				////cout << "NEXT_RECV read" << endl;
				if(buf_long.find("\r\n\r\n") != string::npos) {
					break;
				}
			}
			////cout << "NEXT_RECV stop" << endl;
			size_t posEnd;
			if((posEnd = buf_long.find("\r\n\r\n")) != string::npos) {
				buf_long.resize(posEnd);
			}
		}
	}
	ManagerClientThread *managerClientThread = NULL;
	parse_command(buf, size, client, 0, buf_long.c_str(), &managerClientThread);
	if(managerClientThread) {
		if(managerClientThread->parseCommand()) {
			ClientThreads.add(managerClientThread);
			managerClientThread->run();
		} else {
			delete managerClientThread;
			close(client);
		}
	} else {
		close(client);
	}
	return 0;
}



void *ssh_accept_thread(void *arg) {
	
	char buf[1024*1024]; 
	int len;
	int res = 0;
	LIBSSH2_CHANNEL *channel = (LIBSSH2_CHANNEL*)arg;

	while(1) {
		int res = libssh2_poll_channel_read(channel, 0);

		if(res) {
			len = libssh2_channel_read(channel, buf, 1024*1024);
			if (LIBSSH2_ERROR_EAGAIN == len) {
				continue;
			} else if (len < 0) {
				libssh2_channel_close(channel);
				return 0;
			}
			if (libssh2_channel_eof(channel)) {
				//remote client disconnected
				libssh2_channel_close(channel);
				break;
			}
			res = parse_command(buf, len, 0, 0, NULL, NULL, channel);
			libssh2_channel_close(channel);
			break;
		} else {
			usleep(100);
			continue;
		}
	}

	return 0;
}

void perror_syslog(const char *msg) {
	char buf[1024];
	strerror_r(errno, buf, 1024);
	syslog(LOG_ERR, "%s:%s\n", msg, buf);
}

void *manager_ssh_(void) {
	const char *keyfile1 = "~/.voipmonitor/id_rsa.pub";
	const char *keyfile2 = "~/.voipmonitor/id_rsa";
		       
	int remote_listenport;	
				       
	enum {	 
	AUTH_NONE = 0,
	AUTH_PASSWORD, 
	AUTH_PUBLICKEY 
	};			     
		       
	LIBSSH2_SESSION *session;      

	int rc, sock = -1, auth = AUTH_NONE;
	struct sockaddr_in sin;
	char *userauthlist;
	LIBSSH2_LISTENER *listener = NULL;
	LIBSSH2_CHANNEL *channel = NULL;

	LIBSSH2_POLLFD *fds = (LIBSSH2_POLLFD*)malloc(sizeof (LIBSSH2_POLLFD));


	/* Connect to SSH server */
	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	sin.sin_family = AF_INET;
	if (INADDR_NONE == (sin.sin_addr.s_addr = inet_addr(ssh_host))) {
		perror_syslog("\tinet_addr");
		return 0;
	}      
	sin.sin_port = htons(ssh_port);
	if (connect(sock, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in)) != 0) {
		syslog(LOG_ERR, "\tfailed to connect!\n");
		return 0;
	}
	/* Create a session instance */
	session = libssh2_session_init();
	if(!session) {
		syslog(LOG_ERR, "\tCould not initialize SSH session!\n");
		return 0;
	}

	libssh2_session_flag(session, LIBSSH2_FLAG_COMPRESS, 1);
       
	/* ... start it up. This will trade welcome banners, exchange keys,
	 * and setup crypto, compression, and MAC layers
	 */
	rc = libssh2_session_handshake(session, sock);
	if(rc) {
		syslog(LOG_ERR, "\tError when starting up SSH session: %d", rc);
		return 0;
	}
	
#if 0       
	/* At this point we havn't yet authenticated.  The first thing to do
	 * is check the hostkey's fingerprint against our known hosts Your app
	 * may have it hard coded, may go to a file, may present it to the
	 * user, that's your call
	 */		    
	fingerprint = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1);
	fprintf(stderr, "Fingerprint: ");
	for(i = 0; i < 20; i++)
		fprintf(stderr, "%02X ", (unsigned char)fingerprint[i]);
	fprintf(stderr, "\n");
#endif

	/* check what authentication methods are available */
	userauthlist = libssh2_userauth_list(session, ssh_username, strlen(ssh_username));
	syslog(LOG_ERR, "\tAuthentication methods: %s", userauthlist);
	if (strstr(userauthlist, "password"))
		auth |= AUTH_PASSWORD;
	if (strstr(userauthlist, "publickey"))
		auth |= AUTH_PUBLICKEY;
      
#if 0 
	/* check for options */
	if(argc > 8) {
		if ((auth & AUTH_PASSWORD) && !strcasecmp(argv[8], "-p"))
			auth = AUTH_PASSWORD;
		if ((auth & AUTH_PUBLICKEY) && !strcasecmp(argv[8], "-k"))
			auth = AUTH_PUBLICKEY;
	}
#endif
   
	if (auth & AUTH_PASSWORD) {
		if (libssh2_userauth_password(session, ssh_username, ssh_password)) {
			syslog(LOG_ERR, "\tAuthentication by password failed.");
			goto shutdown2;
		}
	} else if (auth & AUTH_PUBLICKEY) {
		if (libssh2_userauth_publickey_fromfile(session, ssh_username, keyfile1, keyfile2, ssh_password)) {
			syslog(LOG_ERR, "\tAuthentication by public key failed!");
			goto shutdown2;
		}
		syslog(LOG_ERR, "\tAuthentication by public key succeeded.");
	} else {
		syslog(LOG_ERR, "\tNo supported authentication methods found!");
		goto shutdown2;
	}
       
	syslog(LOG_ERR, "\tAsking server to listen on remote %s:%d", ssh_remote_listenhost, ssh_remote_listenport);
	listener = libssh2_channel_forward_listen_ex(session, ssh_remote_listenhost, ssh_remote_listenport, &remote_listenport, 1);
	if (!listener) {
		syslog(LOG_ERR, "\tCould not start the tcpip-forward listener! Note that this can be a problem at the server! Please review the server logs.)");
		goto shutdown2; 
	}       
       
	syslog(LOG_ERR, "\tServer is listening on %s:%d", ssh_remote_listenhost, remote_listenport);
	syslog(LOG_ERR, "\tWaiting for remote connection");

	pthread_t threads;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	/* set the thread detach state */
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	libssh2_session_set_blocking(session, 0); 

	while(1) {

		fds[0].type = LIBSSH2_POLLFD_LISTENER;
		fds[0].fd.listener = listener;
		fds[0].events = LIBSSH2_POLLFD_POLLIN | LIBSSH2_POLLFD_POLLERR | LIBSSH2_POLLFD_SESSION_CLOSED | LIBSSH2_POLLFD_POLLHUP | LIBSSH2_POLLFD_POLLNVAL | LIBSSH2_POLLFD_POLLEX;

		int rc = (libssh2_poll(fds, 1, 100));
		int lastserr;
		lastserr = libssh2_session_last_errno(session);
		if (rc < 1) {
			continue;
		}

		if (fds[0].revents & LIBSSH2_POLLFD_POLLIN) {
			channel = libssh2_channel_forward_accept(listener);
			if (!channel) {
				char errb[1024] = "";
				int len;
				syslog(LOG_ERR, "Could not accept connection! Note that this can be a problem at the server! Please review the server logs.");
				libssh2_session_last_error(session, (char**)&errb, &len, 0);
				syslog(LOG_ERR, "\terr:[%s]\n", errb);
				continue;
			}      
			pthread_create (					/* Create a child thread		*/
				&threads,			       /* Thread ID (system assigned)  */
				&attr,			     /* Default thread attributes */
				ssh_accept_thread,		     /* Thread routine		       */
				channel);
		} else {
			break;
		}
	}
		       
	if (listener)	  
		libssh2_channel_forward_cancel(listener);

	libssh2_session_disconnect(session, "Client disconnecting normally");
	libssh2_session_free(session);
		       
shutdown2:     
	close(sock);   

	libssh2_exit();
	if(fds) free(fds);
	       
	return 0;
}

void *manager_ssh(void *arg) {
	while(1 && terminating == 0) {
		syslog(LOG_NOTICE, "Starting reverse SSH connection service\n");
		manager_ssh_();
		syslog(LOG_NOTICE, "SSH service stopped.\n");
		sleep(1);
	}
}


void *manager_server(void *dummy) {
	sockaddr_in sockName;
	sockaddr_in clientInfo;
	socklen_t addrlen;

	// Vytvorime soket - viz minuly dil
	if ((manager_socket_server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		cerr << "Cannot create manager tcp socket" << endl;
		return 0;
	}
	sockName.sin_family = AF_INET;
	sockName.sin_port = htons(opt_manager_port);
	//sockName.sin_addr.s_addr = INADDR_ANY;
	sockName.sin_addr.s_addr = inet_addr(opt_manager_ip);
	int on = 1;
	setsockopt(manager_socket_server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
tryagain:
	if (bind(manager_socket_server, (sockaddr *)&sockName, sizeof(sockName)) == -1) {
		syslog(LOG_ERR, "Cannot bind to port [%d] trying again after 5 seconds intervals\n", opt_manager_port);
		sleep(5);
		goto tryagain;
	}
	// create queue with 100 connections max 
	if (listen(manager_socket_server, 100) == -1) {
		cerr << "Cannot create manager queue" << endl;
		return 0;
	}
	unsigned int ids;
	pthread_t threads;
	pthread_attr_t        attr;
	pthread_attr_init(&attr);
	/* set the thread detach state */
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	while(1 && terminating == 0) {
		addrlen = sizeof(clientInfo);
		int client = accept(manager_socket_server, (sockaddr*)&clientInfo, &addrlen);
		if(terminating == 1) {
			close(client);
			close(manager_socket_server);
			return 0;
		}
		if (client == -1) {
			//cerr << "Problem with accept client" <<endl;
			close(client);
			continue;
		}

		ids = client;
		pthread_create (                    /* Create a child thread        */
			&threads,                /* Thread ID (system assigned)  */    
			&attr,                   /* Default thread attributes    */
			manager_read_thread,               /* Thread routine               */
			&ids);                   /* Arguments to be passed       */
	}
	close(manager_socket_server);
	return 0;
}

void livesnifferfilter_s::updateState() {
	state_s new_state; 
	new_state.all_saddr = true;
	new_state.all_daddr = true;
	new_state.all_bothaddr = true;
	new_state.all_srcnum = true;
	new_state.all_dstnum = true;
	new_state.all_bothnum = true;
	new_state.all_siptypes = true;
	for(int i = 0; i < MAXLIVEFILTERS; i++) {
		if(this->lv_saddr[i]) {
			new_state.all_saddr = false;
		}
		if(this->lv_daddr[i]) {
			new_state.all_daddr = false;
		}
		if(this->lv_bothaddr[i]) {
			new_state.all_bothaddr = false;
		}
		if(this->lv_srcnum[i][0]) {
			new_state.all_srcnum = false;
		}
		if(this->lv_dstnum[i][0]) {
			new_state.all_dstnum = false;
		}
		if(this->lv_bothnum[i][0]) {
			new_state.all_bothnum = false;
		}
		if(this->lv_siptypes[i]) {
			new_state.all_siptypes = false;
		}
	}
	new_state.all_addr = new_state.all_saddr && new_state.all_daddr && new_state.all_bothaddr;
	new_state.all_num = new_state.all_srcnum && new_state.all_dstnum && new_state.all_bothnum;
	new_state.all_all = new_state.all_addr && new_state.all_num && new_state.all_siptypes;
	this->state = new_state;
}

void updateLivesnifferfilters() {
	livesnifferfilter_use_siptypes_s new_livesnifferfilterUseSipTypes;
	memset(&new_livesnifferfilterUseSipTypes, 0, sizeof(new_livesnifferfilterUseSipTypes));
	if(usersniffer.size()) {
		global_livesniffer = 1;
		map<unsigned int, livesnifferfilter_t*>::iterator usersnifferIT;
		for(usersnifferIT = usersniffer.begin(); usersnifferIT != usersniffer.end(); ++usersnifferIT) {
			usersnifferIT->second->updateState();
			if(usersnifferIT->second->state.all_siptypes) {
				new_livesnifferfilterUseSipTypes.u_invite = true;
				new_livesnifferfilterUseSipTypes.u_register = true;
				new_livesnifferfilterUseSipTypes.u_options = true;
				new_livesnifferfilterUseSipTypes.u_subscribe = true;
				new_livesnifferfilterUseSipTypes.u_message = true;
			} else {
				for(int i = 0; i < MAXLIVEFILTERS; i++) {
					if(usersnifferIT->second->lv_siptypes[i]) {
						switch(usersnifferIT->second->lv_siptypes[i]) {
						case INVITE:
							new_livesnifferfilterUseSipTypes.u_invite = true;
							break;
						case REGISTER:
							new_livesnifferfilterUseSipTypes.u_register = true;
							break;
						case OPTIONS:
							new_livesnifferfilterUseSipTypes.u_options = true;
							break;
						case SUBSCRIBE:
							new_livesnifferfilterUseSipTypes.u_subscribe = true;
							break;
						case MESSAGE:
							new_livesnifferfilterUseSipTypes.u_message = true;
							break;
						}
					}
				}
			}
		}
	} else {
		global_livesniffer = 0;
	}
	livesnifferfilterUseSipTypes = new_livesnifferfilterUseSipTypes;
	/*
	cout << "livesnifferfilterUseSipTypes" << endl;
	if(livesnifferfilterUseSipTypes.u_invite) cout << "INVITE" << endl;
	if(livesnifferfilterUseSipTypes.u_register) cout << "REGISTER" << endl;
	if(livesnifferfilterUseSipTypes.u_options) cout << "OPTIONS" << endl;
	if(livesnifferfilterUseSipTypes.u_subscribe) cout << "SUBSCRIBE" << endl;
	if(livesnifferfilterUseSipTypes.u_message) cout << "MESSAGE" << endl;
	*/
}

bool cmpCallBy_destroy_call_at(Call* a, Call* b) {
	return(a->destroy_call_at < b->destroy_call_at);   
}
bool cmpCallBy_first_packet_time(Call* a, Call* b) {
	return(a->first_packet_time < b->first_packet_time);   
}


ManagerClientThread::ManagerClientThread(int client, const char *type, const char *command, int commandLength) {
	this->client = client;
	this->type = type;
	if(commandLength) {
		this->command = string(command, commandLength);
	} else {
		this->command = command;
	}
	this->finished = false;
	this->_sync_responses = 0;
}

void ManagerClientThread::run() {
	unsigned int counter = 0;
	bool disconnect = false;
	int flag = 0;
	setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
	int flushBuffLength = 1000;
	char *flushBuff = new char[flushBuffLength];
	memset(flushBuff, '_', flushBuffLength - 1);
	flushBuff[flushBuffLength - 1] = '\n';
	while(true && !terminating && !disconnect) {
		string rsltString;
		this->lock_responses();
		if(this->responses.size()) {
			rsltString = this->responses.front();
			this->responses.pop();
		}
		this->unlock_responses();
		if(!rsltString.empty()) {
			if(send(client, rsltString.c_str(), rsltString.length(), 0) == -1) {
				disconnect = true;
			} else {
				send(client, flushBuff, flushBuffLength, 0);
			}
		}
		++counter;
		if((counter % 5) == 0 && !disconnect) {
			if(send(client, "ping\n", 5, 0) == -1) {
				disconnect = true;
			}
		}
		usleep(100000);
	}
	close(client);
	finished = true;
	delete [] flushBuff;
}

ManagerClientThread_screen_popup::ManagerClientThread_screen_popup(int client, const char *command, int commandLength) 
 : ManagerClientThread(client, "screen_popup", command, commandLength) {
	auto_popup = false;
	non_numeric_caller_id = false;
}

bool ManagerClientThread_screen_popup::parseCommand() {
	ClientThreads.cleanup();
	return(this->parseUserPassword());
}

void ManagerClientThread_screen_popup::onCall(int sipResponseNum, const char *callerName, const char *callerNum, const char *calledNum,
					      unsigned int sipSaddr, unsigned int sipDaddr) {
	/*
	cout << "** call 01" << endl;
	cout << "** - called num : " << calledNum << endl;
	struct in_addr _in;
	_in.s_addr = sipSaddr;
	cout << "** - src ip : " << inet_ntoa(_in) << endl;
	cout << "** - reg_match : " << reg_match(calledNum, this->dest_number.empty() ? this->username.c_str() : this->dest_number.c_str()) << endl;
	cout << "** - check ip : " << this->src_ip.checkIP(htonl(sipSaddr)) << endl;
	*/
	if(!(reg_match(calledNum, this->dest_number.empty() ? this->username.c_str() : this->dest_number.c_str()) &&
	     (this->non_numeric_caller_id ||
	      this->isNumericId(calledNum)) &&
	     this->src_ip.checkIP(htonl(sipSaddr)))) {
		return;
	}
	if(this->regex_check_calling_number.size()) {
		bool callerNumOk = false;
		for(size_t i = 0; i < this->regex_check_calling_number.size(); i++) {
			if(reg_match(callerNum, this->regex_check_calling_number[i].c_str())) {
				callerNumOk = true;
				break;
			}
		}
		if(!callerNumOk) {
			return;
		}
	}
	char rsltString[4096];
	char sipSaddrIP[18];
	char sipDaddrIP[18];
	struct in_addr in;
	in.s_addr = sipSaddr;
	strcpy(sipSaddrIP, inet_ntoa(in));
	in.s_addr = sipDaddr;
	strcpy(sipDaddrIP, inet_ntoa(in));
	string callerNumStr = callerNum;
	for(size_t i = 0; i < this->regex_replace_calling_number.size(); i++) {
		string temp = reg_replace(callerNumStr.c_str(), 
					  this->regex_replace_calling_number[i].pattern.c_str(), 
					  this->regex_replace_calling_number[i].replace.c_str());
		if(!temp.empty()) {
			callerNumStr = temp;
		}
	}
	sprintf(rsltString,
		"call_data: "
		"sipresponse:[[%i]] "
		"callername:[[%s]] "
		"caller:[[%s]] "
		"called:[[%s]] "
		"sipcallerip:[[%s]] "
		"sipcalledip:[[%s]]\n",
		sipResponseNum,
		callerName,
		callerNumStr.c_str(),
		calledNum,
		sipSaddrIP,
		sipDaddrIP);
	this->lock_responses();
	this->responses.push(rsltString);
	this->unlock_responses();
}

bool ManagerClientThread_screen_popup::parseUserPassword() {
	char user[128];
	char password[128];
	char key[128];
	sscanf(command.c_str(), "login_screen_popup %s %s %s", user, password, key);
	string password_md5 = GetStringMD5(password);
	SqlDb *sqlDb = createSqlObject();
	sqlDb->query(
		string(
		"select u.username,\
			u.name,\
			u.dest_number,\
			u.allow_change_settings,\
			p.name as profile_name,\
			p.auto_popup,\
			p.show_ip,\
			p.popup_on,\
			p.non_numeric_caller_id,\
			p.regex_calling_number,\
			p.src_ip_whitelist,\
			p.src_ip_blacklist,\
			p.app_launch,\
			p.app_launch_args_or_url,\
			p.popup_title\
		 from screen_popup_users u\
		 join screen_popup_profile p on (p.id=u.profile_id)\
		 where username=") +
		sqlEscapeStringBorder(user) +
		" and password=" + 
		sqlEscapeStringBorder(password_md5));
	SqlDb_row row = sqlDb->fetchRow();
	char rsltString[4096];
	bool rslt;
	if(row) {
		rslt = true;
		username = row["username"];
		name = row["name"];
		dest_number = row["dest_number"];
		allow_change_settings = atoi(row["allow_change_settings"].c_str());
		profile_name = row["profile_name"];
		auto_popup = atoi(row["auto_popup"].c_str());
		show_ip = atoi(row["show_ip"].c_str());
		popup_on = row["popup_on"];
		non_numeric_caller_id = atoi(row["non_numeric_caller_id"].c_str());
		if(!row["regex_calling_number"].empty()) {
			vector<string> items = split(row["regex_calling_number"].c_str(), split("\r|\n", "|"), true);
			for(size_t i = 0; i < items.size(); i++) {
				vector<string> itemItems = split(items[i].c_str(), "|", true);
				if(itemItems.size() == 2) {
					this->regex_replace_calling_number.push_back(RegexReplace(itemItems[0].c_str(), itemItems[1].c_str()));
				} else {
					this->regex_check_calling_number.push_back(itemItems[0]);
				}
			}
		}
		src_ip.addWhite(row["src_ip_whitelist"].c_str());
		src_ip.addBlack(row["src_ip_blacklist"].c_str());
		app_launch = row["app_launch"];
		app_launch_args_or_url = row["app_launch_args_or_url"];
		popup_title = row["popup_title"];
		if(!opt_php_path[0]) {
			rslt = false;
			strcpy(rsltString, "login_failed error:[[Please set php_path parameter in voipmonitor.conf.]]\n");
		} else {
			string cmd = string("php ") + opt_php_path + "/php/run.php checkScreenPopupLicense -k " + key;
			FILE *fp = popen(cmd.c_str(), "r");
			if(fp == NULL) {
				rslt = false;
				strcpy(rsltString, "login_failed error:[[Failed to run php checkScreenPopupLicense.]]\n");
			} else {
				char rsltFromPhp[1024];
				if(!fgets(rsltFromPhp, sizeof(rsltFromPhp) - 1, fp)) {
					rslt = false;
					strcpy(rsltString, "login_failed error:[[License check failed please contact support.]]\n");
				} else if(!strncmp(rsltFromPhp, "error: ", 7)) {
					rslt = false;
					strcpy(rsltString, (string("login_failed error:[[") + (rsltFromPhp + 7) + "]]\n").c_str());
				} else {
					char key[1024];
					int maxClients;
					if(sscanf(rsltFromPhp, "key: %s max_clients: %i", key, &maxClients) == 2) {
						if(maxClients && ClientThreads.getCount() >= maxClients) {
							rslt = false;
							strcpy(rsltString, "login_failed error:[[Maximum connection limit reached.]]\n");
						} else {
							sprintf(rsltString, 
								"login_ok "
								"auto_popup:[[%i]] "
								"popup_on_200:[[%i]] "
								"popup_on_18:[[%i]] "
								"show_ip:[[%i]] "
								"app_launch:[[%s]] "
								"args_or_url:[[%s]] "
								"key:[[%s]] "
								"allow_change_settings:[[%i]] "
								"popup_title:[[%s]]\n", 
								auto_popup, 
								popup_on == "200" || popup_on == "183/180_200",
								popup_on == "183/180" || popup_on == "183/180_200",
								show_ip, 
								app_launch.c_str(), 
								app_launch_args_or_url.c_str(), 
								key, 
								allow_change_settings,
								popup_title.c_str());
						}
					} else {
						rslt = false;
							strcpy(rsltString, "login_failed error:[[License is invalid.]]\n");
					}
				}
				pclose(fp);
			}
		}
	} else {
		rslt = false;
		strcpy(rsltString, "login_failed error:[[Invalid user or password.]]\n");
	}
	delete sqlDb;
	send(client, rsltString, strlen(rsltString), 0);
	return(rslt);
}

bool ManagerClientThread_screen_popup::isNumericId(const char *id) {
	while(*id) {
		if(!isdigit(*id) &&
		   *id != ' ' &&
		   *id != '+') {
			return(false);
		}
		++id;
	}
	return(true);
}

ManagerClientThreads::ManagerClientThreads() {
	_sync_client_threads = 0;
}
	
void ManagerClientThreads::add(ManagerClientThread *clientThread) {
	this->lock_client_threads();
	clientThreads.push_back(clientThread);
	this->unlock_client_threads();
	this->cleanup();
}

void ManagerClientThreads::onCall(int sipResponseNum, const char *callerName, const char *callerNum, const char *calledNum,
				  unsigned int sipSaddr, unsigned int sipDaddr) {
	this->lock_client_threads();
	vector<ManagerClientThread*>::iterator iter;
	for(iter = this->clientThreads.begin(); iter != this->clientThreads.end(); ++iter) {
		(*iter)->onCall(sipResponseNum, callerName, callerNum, calledNum, sipSaddr, sipDaddr);
	}
	this->unlock_client_threads();
}

void ManagerClientThreads::cleanup() {
	this->lock_client_threads();
	for(int i = this->clientThreads.size() - 1; i >=0; i--) {
		ManagerClientThread *ct = this->clientThreads[i];
		if(ct->isFinished()) {
			delete ct;
			this->clientThreads.erase(this->clientThreads.begin() + i);
			
		}
	}
	this->unlock_client_threads();
}

int ManagerClientThreads::getCount() {
	this->lock_client_threads();
	int count = this->clientThreads.size();
	this->unlock_client_threads();
	return(count);
}
