#include <string.h>

#include <string>
#include <map>

// external lib
#include <boost/function.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/noncopyable.hpp>
#include <boost/unordered_map.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <boost/timer.hpp>
#include <boost/random.hpp>
#include <boost/thread.hpp>
#include <boost/mem_fn.hpp>

// msgpack
#include <msgpack/rpc/server.h>
#include <msgpack/rpc/client.h>
#include <cclog/cclog.h>
#include <cclog/cclog_tty.h>
//#include <glog/logging.h>

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
	node(node&& org):obj_(org.obj_),zone_(org.zone_){}
};
typedef boost::shared_ptr<node> shared_node;

struct node_animate: public node{
	uint32_t lifespan;
	boost::timer timer;
	node_animate(const msgpack::object& obj, msgpack::rpc::auto_zone& zone
							 , uint32_t life)
		:node(obj,zone),lifespan(life){}
	node_animate(node_animate&& org)// move semantics
		:node(org.obj_,org.zone_),lifespan(org.lifespan), timer(org.timer){}
	node_animate(node&& org, uint32_t life)
		:node(org),lifespan(life){}
	bool is_live()const{return lifespan < timer.elapsed();}
};

typedef boost::shared_ptr<node_animate> shared_node_animate;

node kill(node_animate& n){
	return node(n.obj(), n.zone());
}

typedef boost::shared_ptr<node_animate> shared_node_animate;

class queue_server : public msgpack::rpc::dispatcher, public boost::noncopyable{
private:
	// storages
	typedef boost::unordered_map<uint32_t,node_animate> queue_animate;
	typedef std::deque<shared_node> queue;
	typedef mp::sync<queue>::ref ref_queue;
	typedef mp::sync<queue_animate>::ref ref_queue_animate;
	mp::sync<queue> sync_queue;
	mp::sync<queue> retry_queue;
	mp::sync<queue_animate> wait_queue;

	// waiting
	struct waiting;
	typedef msgpack::rpc::request request;
	typedef std::deque<waiting*> req_queue;
	typedef mp::sync<req_queue>::ref ref_reqests;
	mp::sync<req_queue> requests;
public:
	queue_server(msgpack::rpc::server* sv)
		:engine(static_cast<unsigned long>(time(NULL)))
		,hash_range(0, INT_MAX),rand(engine,hash_range)
		,life_checker(boost::mem_fn(&queue_server::life_check))
	{}
	void dispatch(msgpack::rpc::request req){
		const std::string& method = req.method().as<std::string>();
		if(strcmp(method.c_str(),"enq")){
			{ // check suspended deque
				ref_reqests rq(requests);
				if(!rq->empty()){// if waiting queue exists
					boost::scoped_ptr<waiting> waited_req(rq->front());

					rq.get_mutex().unlock();
					if(waited_req->is_borrow()){
						ref_queue_animate wq(wait_queue);
						const uint32_t artifact = search_unused_hash(wq);
						node_animate newnode
							(req.params(),req.zone(),waited_req->life());
						std::pair<queue_animate::iterator, bool> res = 
							wq->insert(std::make_pair(artifact,newnode));
						assert(res.second);
						waited_req->req().result<borrow_return>
							(borrow_return(res.first->second.obj(),artifact));
						rq->pop_front();
					}else{
						waited_req->req().result(req.params());
						rq->pop_front();
					}
					return;
				}
			}
			while(1){
				const shared_node newnode(new node(req.params(), req.zone()));
				std::string arg = req.params().as<std::string>();
				ref_queue sq(sync_queue);
				sq->push_back(shared_node());
				break;
			}
		}
		else if(strcmp(method.c_str(),"deq")){
			const shared_node qued = deque_node();
			if(qued){ // send dequed object
				req.result<msgpack::object>(qued->obj()); // send dequed object
			}else{// push deque task
				ref_reqests rr(requests);
				rr->push_back(new waiting_deque(req));
			}
		}
		else if(strcmp(method.c_str(), "borrow")){
			const uint32_t time = req.params().as<uint32_t>();
			shared_node qued = deque_node();
			if(qued){ // send dequed object
				uint32_t artifact;
				{// push for lifetime management
					ref_queue_animate wq(wait_queue);
					artifact = search_unused_hash(wq);
					std::pair<queue_animate::iterator, bool> res = 
						wq->insert(std::make_pair(artifact,node_animate(*qued,time)));
					assert(res.second);
					req.result<borrow_return>
						(borrow_return
						 (res.first->second.obj(), artifact));
				}
			}else{// there is no node to deque, so push deque task
				ref_reqests rr(requests);
				rr->push_back(new waiting_borrow(req,time)); // push deque task for client
			}
		}
		else if(strcmp(method.c_str(), "commit")){
			const uint32_t artifact = req.params().as<uint32_t>();
			bool result;
			{
				ref_queue_animate wq(wait_queue);
				queue_animate::iterator it = wq->find(artifact);
				if(it == wq->end()){ result = false;}
				else{ wq->erase(it); result = true;}
			}
			req.result(result);
			return;
		}
		else if(strcmp(method.c_str(), "cancel")){
			
		}
	}
private: // method
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
	uint32_t search_unused_hash(ref_queue_animate& wq){
		while(1){
			const uint32_t random = rand();
			if(wq->find(random) == wq->end()){return random;};
		}
	}
	void life_check(){
		while(1){
			// one iterate for one second
			sleep(1);
			{ // if borrowed queue is expired, returns back to queue
				ref_queue_animate rq(wait_queue);
				queue_animate::iterator it = rq->begin();
				while(it != rq->end()){
					if(!it->second.is_live()){
						node killed = kill(it->second);
						rq->erase(it);
						{ // entry again, because borrowed node is not commited
							ref_queue rt(retry_queue);
							rt->push_back(shared_node(new node(killed)));
						}
					}else{
						++it;
					}
				}
			}
		}
	}
private: // class
	struct borrow_return{ // only binding msgpack::object and uint32_t
		msgpack::object obj_;
		uint32_t artifact_;
		borrow_return(const msgpack::object& obj, uint32_t artifact)
			:obj_(obj), artifact_(artifact){}
		MSGPACK_DEFINE(obj_, artifact_);
	};
	struct waiting{
	private:
		request req_;
	public:
		waiting(const request& req):req_(req){}
		request& req(){return req_;}
		virtual ~waiting()=0;
		virtual bool is_borrow()const = 0;
		virtual uint32_t& life()=0;
	};
	struct waiting_deque:  public waiting{
		waiting_deque(const request& req):waiting(req){}
		~waiting_deque(){}
		bool is_borrow()const{return false;}
		uint32_t& life(){ abort();}
	};
	struct waiting_borrow: public waiting{
	private:
		uint32_t life_;
	public:
		waiting_borrow(const request& req, uint32_t life)
			:waiting(req), life_(life){}
		~waiting_borrow(){}
		bool is_borrow()const{return true;}
		uint32_t& life(){return life_;}
	};

private: // member
	// random query

	boost::mt19937 engine;
	boost::uniform_smallint<> hash_range;
	mutable boost::variate_generator
	<boost::mt19937&, boost::uniform_smallint<> > rand;
	boost::thread life_checker;
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
