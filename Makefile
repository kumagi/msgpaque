CXX=g++
OPTS=-std=c++0x -fexceptions -g -O0
LD=-L/usr/local/lib -lboost_program_options -lmsgpack -lmpio -lglog
PATH_MSGPACK_RPC=../msgpack-rpc/cpp/src/msgpack/rpc
TEST_LD= -lpthread $(LD)
CCLOG_LD=../msgpack-rpc/cpp/src/cclog/*.o
GTEST_INC= -I$(GTEST_DIR)/include -I$(GTEST_DIR)
GTEST_DIR=/opt/google/gtest-1.5.0
GMOCK_DIR=/opt/google/gmock-1.5.0
WARNS= -W -Wall -Wextra -Wformat=2 -Wstrict-aliasing=4 -Wcast-qual -Wcast-align \
	-Wwrite-strings -Wfloat-equal -Wpointer-arith -Wswitch-enum
NOTIFY=&& notify-send Test success! -i ~/themes/ok_icon.png || notify-send Test failed... -i ~/themes/ng_icon.png
SRCS=$(HEADS) $(BODYS)
MSGPACK_RPC_OBJS=$(PATH_MSGPACK_RPC)/*.o

target:msgpaque
#target:test

msgpaque:msgpaque.o
	$(CXX) $^ -o $@ $(LD) $(OPTS) $(WARNS)  $(PATH_MSGPACK_RPC)/*.o -I$(PATH_MSGPACK_RPC)/ $(CCLOG_LD)
msgpaque.o:msgpaque.cc
	$(CXX) -c $^ -o $@ $(OPTS) $(WARNS)  $(PATH_MSGPACK_RPC)/*.o -I$(PATH_MSGPACK_RPC)/
test:test.cc
	$(CXX) $^ -o $@ $(GTEST_INC) $(TEST_LD) $(OPTS) $(WARNS) $(PATH_MSGPACK_RPC)/*.o -I$(PATH_MSGPACK_RPC)/  $(CCLOG_LD)
	./logic_test $(NOTIFY)

#obj_eval.i: obj_eval.hpp
#	$(CXX) -E obj_eval.hpp -o $@

#%.o: %.c
#	$(CXX) -c $(OPTS) $(WARNS) $< -o $@
# gtest
gtest_main.o:
	$(CXX) $(GTEST_INC) -c $(OPTS) $(GTEST_DIR)/src/gtest_main.cc -o $@
gtest-all.o:
	$(CXX) $(GTEST_INC) -c $(OPTS) $(GTEST_DIR)/src/gtest-all.cc -o $@
gtest_main.a:gtest-all.o gtest_main.o
	ar -r $@ $^

libgmock.a:
	g++ ${GTEST_INC} -I${GTEST_DIR} -I${GMOCK_DIR}/include -I${GMOCK_DIR} -c ${GTEST_DIR}/src/gtest-all.cc
	g++ ${GTEST_INC} -I${GTEST_DIR} -I${GMOCK_DIR}/include -I${GMOCK_DIR} -c ${GMOCK_DIR}/src/gmock-all.cc
	ar -rv libgmock.a gtest-all.o gmock-all.o

clean:
	rm -f *.o
	rm -f *~
