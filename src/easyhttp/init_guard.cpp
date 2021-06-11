#include "init_guard.hpp"

#include <thread>
#include <map>

#include <curl/curl.h>
#include <nitki/queue.hpp>
#include <nitki/semaphore.hpp>

using namespace easyhttp;

decltype(init_guard::instance) init_guard::instance;

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

void init_guard::handle_completed_request(const void* CURLMsg_message){
	const CURLMsg& m = *reinterpret_cast<const CURLMsg*>(CURLMsg_message);
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

			long response_code;
    		curl_easy_getinfo(r->CURL_handle, CURLINFO_RESPONSE_CODE, &response_code);
			r->resp.response_code = http_code(response_code);

			if(r->completed_handler){
				r->completed_handler(*r);
			}
			break;
		}
		default:
			ASSERT_INFO(false, "m.msg = " << m.msg)
			break;
	}
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
				init_guard::handle_completed_request(msg);
			}
		}while(msg);

		long timeout;
 
		curl_multi_timeout(multi_handle, &timeout);
		if(timeout < 0){ // no set timeout, use default
			timeout = 1000;
		}

      	CURLMcode rc = curl_multi_poll(multi_handle, nullptr, 0, timeout, nullptr);
 
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
		curl_multi_add_handle(multi_handle, r->CURL_handle);
		handle_to_request_map.insert(std::make_pair(r->CURL_handle, std::move(r)));
	});
	curl_multi_wakeup(multi_handle);
}

bool init_guard::cancel_request(request& r){
	// TRACE(<< "cancelling request..." << std::endl)
	nitki::semaphore sema;
	bool ret;
	queue.push_back([&r, &sema, &ret](){
		auto i = handle_to_request_map.find(r.CURL_handle);
		if(i == handle_to_request_map.end()){
			// TRACE(<< "request is not active" << std::endl)
			ret = false;
		}else{
			curl_multi_remove_handle(multi_handle, r.CURL_handle);
			handle_to_request_map.erase(i);
			r.is_idle = true;
			ret = true;
			// TRACE(<< "request removed from handling" << std::endl)
		}
		sema.signal();
	});
	curl_multi_wakeup(multi_handle);
	sema.wait();
	// TRACE(<< "request cancelled..." << std::endl)
	return ret;
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
