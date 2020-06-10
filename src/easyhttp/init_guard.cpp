#include "init_guard.hpp"

#include <thread>
#include <map>

#include <curl/curl.h>
#include <nitki/queue.hpp>

using namespace easyhttp;

namespace{
std::thread thread;
volatile bool quit_flag;
nitki::queue queue;
CURLM *multi_handle = nullptr;
std::map<CURL*, std::shared_ptr<request>> handle_to_request_map;
}

namespace{
status_code curlcode_to_status(CURLcode code){
	switch(code){
		case CURLE_OK:
			return status_code::ok;
		default:
			TRACE(<< "CURLcode = " << code << std::endl)
			return status_code::network_error;
	}
}
}

void init_guard::handle_completed_request(const CURLMsg& m){
	switch(m.msg){
		case CURLMSG_DONE:
		{
			CURL* handle = m.easy_handle;
			curl_multi_remove_handle(multi_handle, handle);
			auto i = handle_to_request_map.find(handle);
			ASSERT(i != handle_to_request_map.end())
			auto r = std::move(i->second);
			handle_to_request_map.erase(i);
			r->is_idle = true;
			r->resp.status = curlcode_to_status(m.data.result);
			if(r->completed_handler){
				r->completed_handler(r->resp);
			}
			break;
		}
		default:
			ASSERT_INFO(false, "m.msg = " << m.msg)
			break;
	}
	// TODO:
}

void init_guard::thread_func(){
	while(!quit_flag){
		while(auto m = queue.pop_front()){
			m();
		}

		int num_active_sockets;
		curl_multi_perform(multi_handle, &num_active_sockets);
 
		// handle completed requests
		CURLMsg* msg;
		do{
			int num_messages_left;
			msg = curl_multi_info_read(multi_handle, &num_messages_left);
			if(msg){
				init_guard::handle_completed_request(*msg);
			}
		}while(msg);

		long timeout;
 
		curl_multi_timeout(multi_handle, &timeout);
		if(timeout < 0){ // no set timeout, use default
			timeout = 1000;
		}

		TRACE(<< "polling" << std::endl)

      	CURLMcode rc = curl_multi_poll(multi_handle, NULL, 0, timeout, nullptr);
 
		if(rc != CURLM_OK){
			TRACE_ALWAYS(<< "curl_multi_poll() failed, terminating easyhttp thread" << std::endl)
			break;
    	}
	}

	curl_multi_cleanup(multi_handle);
	multi_handle = nullptr;

	curl_global_cleanup();
}

void init_guard::start_request(std::shared_ptr<request> r){
	queue.push_back([r](){
		curl_multi_add_handle(multi_handle, r->handle);
		handle_to_request_map.insert(std::make_pair(r->handle, std::move(r)));
	});
	curl_multi_wakeup(multi_handle);
}

init_guard::init_guard(bool init_winsock){
	long flags = CURL_GLOBAL_SSL;
	if(init_winsock){
		flags |= CURL_GLOBAL_WIN32;
	}
	curl_global_init(flags);

	multi_handle = curl_multi_init();

	quit_flag = false;
	thread = std::thread(&thread_func);
}

init_guard::~init_guard()noexcept{
	quit_flag = true;
	ASSERT(multi_handle)
	curl_multi_wakeup(multi_handle);
	thread.join();
}