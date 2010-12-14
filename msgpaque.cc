#include <string.h>

#include <string>
#include <map>
#include <fstream>

// external lib
#include <boost/function.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/noncopyable.hpp>
#include <boost/interprocess/smart_ptr/unique_ptr.hpp>
#include <boost/unordered_map.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <boost/timer.hpp>
#include <boost/random.hpp>
#include <boost/thread.hpp>
#include <boost/mem_fn.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/bind.hpp>
#include <boost/mem_fn.hpp>


// msgpack
#include <msgpack/rpc/server.h>
#include <msgpack/rpc/client.h>
#include "fifo.hpp"

#define my_assert(cond)	for (; !(cond); assert(#cond == false))

using namespace mp::placeholders;
namespace rpc { using namespace msgpack::rpc; }

struct node{
	const msgpack::object obj_;
	msgpack::rpc::auto_zone zone_;
	const msgpack::object& obj()const{return obj_;}
	msgpack::rpc::auto_zone& zone(){return zone_;}
	size_t get_size()const{return zone_->chunk_size;}
	node(const msgpack::object& obj, msgpack::rpc::auto_zone& zone)
		:obj_(obj),zone_(zone){}
};
typedef boost::shared_ptr<node> shared_node;

struct cmdline : public boost::noncopyable{
	const std::string interface;
	const uint16_t port;
	const uint32_t thread;
	const uint32_t memory_in_mb;
	cmdline(const std::string& i, const uint16_t& p, const uint32_t& t, const uint32_t m)
		:interface(i), port(p), thread(t), memory_in_mb(m){}
private:
};

class queue_server : public msgpack::rpc::dispatcher, public boost::noncopyable{
private:
	// storages
	typedef boost::lockfree::fifo<node*> queue;
	queue ready_to_deque;
	queue buff_in_file;
	queue file_to_ready;
	typedef boost::lockfree::fifo<msgpack::rpc::request*> reserve_queue;
	reserve_queue reserve;

	
	// status
	typedef boost::shared_lock<boost::shared_mutex> read_lock;
	typedef boost::upgrade_lock<boost::shared_mutex> prepare_write_lock;
	typedef boost::upgrade_to_unique_lock<boost::shared_mutex> write_lock;
	enum {
		on_memory = 0,
		on_disk = 1,
	};
	
	uint8_t target_state; // lock protected
	uint64_t memory_in_use; // lock protected
	boost::shared_mutex state_mutex;
	
	// random
	boost::mt19937 engine;
	boost::uniform_smallint<> range;
	mutable boost::variate_generator<boost::mt19937&, boost::uniform_smallint<> > rand;

	// threads
	boost::thread buff2file,file2ready;

	// files
	typedef boost::unique_lock<boost::mutex> unique_lock;
	boost::filesystem::fstream tmpfile;
	boost::mutex file_mutex;

	// settings
	const cmdline& setting;
public:
	queue_server(const cmdline& setting_)
		:memory_in_use(sizeof(queue_server))
		, engine(static_cast<uint32_t>(time(0))), range(0,INT_MAX), rand(engine, range)
		, file2ready(boost::bind(&queue_server::disk_to_ready, this))
		, tmpfile("tmp")
		, setting(setting_)
	{}
	
	void dispatch(msgpack::rpc::request req){
		const std::string& method = req.method().as<std::string>();
		if(method == "enq"){
			std::auto_ptr<node> newnode(new node(req.params(), req.zone()));
			int target;
			{
				msgpack::rpc::request* waiting = NULL;
				if(reserve.dequeue(&waiting)){
					std::auto_ptr<msgpack::rpc::request> waited_req(waiting);
					waited_req->result<msgpack::object>(newnode->obj(), newnode->zone());
					return;
				}
			}
			{
				prepare_write_lock lk(state_mutex);
				if(memory_in_use + newnode->get_size() < setting.memory_in_mb * 1024*1024
					 && target_state == on_memory){
					write_lock wlk(lk);
					target_state = on_disk;
				}else if(target_state == on_memory){
					write_lock wlk(lk);
					memory_in_use += newnode->get_size();
				}
				target = target_state;
			}
			switch (target){
			case on_memory:{
				ready_to_deque.enqueue(newnode.get());
				newnode.release();
			}
			case on_disk:{
				msgpack::sbuffer sbuf;
				msgpack::pack(sbuf, newnode->obj());
				// write sbuf.data for sbuf.size bytes
				{
					unique_lock lk(file_mutex);
					const uint64_t size = sbuf.size();
					tmpfile.write(reinterpret_cast<const char*>(&size), sizeof(uint64_t));
					tmpfile.write(sbuf.data(), sbuf.size());
				}
			}
			}
			req.result(0);
		}
		else if(method == "deq"){
			node* dequed = NULL;
			while(1){  // for retrying
				if(ready_to_deque.dequeue(&dequed)){ // from memory
					std::auto_ptr<node> comingnode(dequed);
					req.result<msgpack::object>(comingnode->obj(), comingnode->zone());
					{
						prepare_write_lock lk(state_mutex);
						write_lock wlk(lk);
						memory_in_use -= comingnode->get_size();
					}
					return;
				}else {
					read_lock lk(state_mutex);
					if(target_state == on_disk){
						// wait for reading data from disk
						usleep(100);continue;
					}
					else{
						lk.unlock();
						// put the request in reservation queue
						reserve.enqueue(new msgpack::rpc::request(req));
						return;
					}
				}
			}
		}
		else {
			req.result(1);
		}
	}
private:
	void disk_to_ready(){
		while(1){
			{
				{
					read_lock lk(state_mutex);
					

			}
		}
	}
};
	
int main(int argc, char** argv){
	boost::scoped_ptr<const cmdline> setting;
	{// parse cmdline
		boost::program_options::options_description opt("options");
		uint16_t p;
		std::string i;
		uint32_t t,m;
		opt.add_options() 
			("help,h", "view help")
			("interface,i",boost::program_options::value<std::string>
			 (&i)->default_value("any"), "my interface")
			("port,p",boost::program_options::value<uint16_t>
			 (&p)->default_value(12321), "my port number")
			("memory,m",boost::program_options::value<uint32_t>
			 (&m)->default_value(4096), "using memory for cache(MiB)")
			("thread,t",boost::program_options::value<uint32_t>
			 (&t)->default_value(2), "number of thread");

 		boost::program_options::variables_map vm;
		store(parse_command_line(argc,argv,opt), vm);
		notify(vm);
		if(vm.count("help")){
			std::cerr << opt << std::endl;
			exit(0);
		}
		setting.reset(new cmdline(i,p,t,m));
	}

	{// launch server
		{
			boost::filesystem::path tmpfile("tmp");
			boost::filesystem::remove(tmpfile);
		}
		queue_server host(*setting); // main callback
		msgpack::rpc::server server;

		server.serve(&host);
		server.listen("0.0.0.0", setting->port);

		server.start(4);
		server.join();// wait for server ends
	}
	return 0;
}
