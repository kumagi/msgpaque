#include <string.h>

#include <string>
#include <map>

// external lib
#include <boost/function.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <boost/timer.hpp>

// msgpack
#include <msgpack/rpc/server.h>
#include <msgpack/rpc/client.h>
#include <cclog/cclog.h>
#include <cclog/cclog_tty.h>
#include <glog/logging.h>

#include <mp/sync.h>

using namespace mp::placeholders;
namespace rpc { using namespace msgpack::rpc; }

struct node{
	const msgpack::object obj_;
	msgpack::rpc::auto_zone zone_;
	const msgpack::object& obj()const{return obj_;}
	msgpack::rpc::auto_zone& zone(){return zone_;}
	node(const msgpack::object& obj, msgpack::rpc::auto_zone& zone)
		:obj_(obj),zone_(zone){}
};
typedef boost::shared_ptr<node> shared_node;

struct node_animate: public node{
	uint32_t lifespan;
	boost::timer timer;
	node_animate(const msgpack::object& obj, msgpack::rpc::auto_zone& zone
							 , uint32_t life)
		:node(obj,zone),lifespan(life){}
	bool is_live()const{return lifespan < timer.elapsed();}
	
};
static boost::shared_ptr<node_animate> 
animate(const shared_node& n, const uint32_t& life){
	return boost::shared_ptr<node_animate>
		(new node_animate(n->obj(),n->zone(),life));
}
typedef boost::shared_ptr<node_animate> shared_node_animate;

class queue_server : public msgpack::rpc::dispatcher, public boost::noncopyable{
public:
	queue_server(msgpack::rpc::server* sv){}
	void dispatch(msgpack::rpc::request req){
		const std::string& method = req.method().as<std::string>();
		
		if(strcmp(method.c_str(),"enq")){
			while(1){
				const shared_node newnode(new node(req.params(), req.zone()));
				std::string arg = req.params().as<std::string>();
				ref_queue sq(sync_queue);
				sq->push_back(shared_node());
				break;
			}
			
		}else if(strcmp(method.c_str(),"deq")){
			const shared_node qued = deque_node();
			if(qued){ // send dequed object
				req.result<msgpack::object>(qued->obj()); // send dequed object
			}else{// push deque task
				ref_reqests rr(requests);
				rr->push_back(req);
			}
			
		}else if(strcmp(method.c_str(), "borrow")){
			const uint32_t time = req.params().as<uint32_t>();
			const shared_node qued = deque_node();
			if(qued){ // send dequed object
				req.result<msgpack::object>(qued->obj());
			}else{// push deque task
				ref_reqests rr(requests);
				rr->push_back(req); // push deque task for client
			}
			{// push for lifetime management
				ref_queue_animate wq(wait_queue);
				wq->push_back(animate(qued,time));
			}

		}else if(strcmp(method.c_str(), "commit")){
			
		}else if(strcmp(method.c_str(), "cancel")){
			
		}
	}
private:
	shared_node deque_node(){
		shared_node qued;
		do{
			if(!retry_queue.unsafe_ref().empty()){
				{// get dequed object from retry queue
					ref_queue rq(retry_queue);
					if(rq->empty()){continue;}
					qued = rq->front();rq->pop_front();
				}
			}else{// get dequed object from normal queue
				ref_queue sq(sync_queue);
				if(sq->empty()){// both queue may be empty
					ref_queue rq(retry_queue);
					if(rq->empty()){ // both queue is empty
						rq.get_mutex().unlock();
						sq.get_mutex().unlock();
					}else{continue;}
					return qued;
				}
				qued = sq->front();sq->pop_front();
			}
			return qued;
		}while(0);
	}
private:
	typedef std::deque<shared_node_animate> queue_animate;
	typedef std::deque<shared_node> queue;
	typedef mp::sync<queue>::ref ref_queue;
	typedef mp::sync<queue_animate>::ref ref_queue_animate;
	mp::sync<queue> sync_queue;
	mp::sync<queue> retry_queue;
	mp::sync<queue_animate> wait_queue;

	typedef msgpack::rpc::request request;
	typedef std::deque<request> req_queue;
	typedef mp::sync<req_queue>::ref ref_reqests;
	mp::sync<req_queue> requests;
};

struct cmdline{
	std::string interface;
	uint16_t port;
	uint32_t thread;
	cmdline(){}
	cmdline(const std::string& i, const uint16_t& p, const uint32_t& t)
		:interface(i), port(p), thread(t){}
private:

};

int main(int argc, char** argv){
	cclog::reset(new cclog_tty(cclog::TRACE, std::cerr));

	boost::scoped_ptr<const cmdline> setting;
	{// parse cmdline
		boost::program_options::options_description opt("options");
		uint16_t p;
		std::string i;
		uint32_t t;
		opt.add_options() 
			("help,h", "view help")
			("interface,i",boost::program_options::value<std::string>
			 (&i)->default_value("any"), "my interface")
			("port,p",boost::program_options::value<uint16_t>
			 (&p)->default_value(12321), "my port number")
			("thread,t",boost::program_options::value<uint32_t>
			 (&t)->default_value(2), "number of thread");
	
 		boost::program_options::variables_map vm;
		store(parse_command_line(argc,argv,opt), vm);
		notify(vm);
		if(vm.count("help")){
			std::cerr << opt << std::endl;
			exit(0);
		}
		setting.reset(new cmdline(i,p,t));
	}

	{// launch server
		msgpack::rpc::server server;
		queue_server host(&server); // main callback

		server.serve(&host);
		server.listen("0.0.0.0", setting->port);

		server.start(4);
		server.join();// wait for server ends
	}
	return 0;
}
