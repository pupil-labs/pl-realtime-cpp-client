BUILD_DIR = build
LIB_DIR = $(BUILD_DIR)/lib
INC_DIR = $(BUILD_DIR)/include
LIVE555_DIR = $(BUILD_DIR)/live555
SRC_DIR = pl-rtsp-service

SONAME = libplrtclient.so
DYNLIB = $(LIB_DIR)/$(SONAME)

SOURCES = $(wildcard $(LOC_SRC_DIR)/*.cpp)
HEADERS = $(wildcard $(LOC_SRC_DIR)/*.h*)

INCLUDES_DIR = -I$(SRC_DIR) \
			   -I$(INC_DIR)/liveMedia \
			   -I$(INC_DIR)/UsageEnvironment \
			   -I$(INC_DIR)/groupsock \
			   -I$(INC_DIR)/BasicUsageEnvironment \
			   -I$(INC_DIR)/EpollTaskScheduler

LINK_DIR = -L$(LIB_DIR)

LINK_FILE = -lliveMedia \
			-lUsageEnvironment \
			-lgroupsock \
			-lBasicUsageEnvironment \
			-lEpollTaskScheduler

CXX := g++
CFLAGS = -Wall -fPIC $(EXT_CFLAGS)

all: $(DYNLIB)

# compile project
$(DYNLIB): $(SOURCES) $(HEADERS)
	mkdir -p $(LIB_DIR)
	$(CXX) -shared $(CFLAGS) -Wl,-soname,$(SONAME) -DNDEBUG -DPLRTSPSERVICE_EXPORTS -DNO_OPENSSL=1 -DLIVEMEDIA_API= $(INCLUDES_DIR) $^ -o $@ $(LINK_DIR) $(LINK_FILE)
	cp -a $(SRC_DIR)/*.h* $(INC_DIR)/.

.PHONY: clean live555-bootstrap

live555-bootstrap:
	mkdir -p $(LIVE555_DIR)
	( cd $(LIVE555_DIR) && cmake -G "Unix Makefiles" -DLIVE555_ENABLE_OPENSSL=ON -DLIVE555_BUILD_EXAMPLES=OFF -DLIVE555_BUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX="$(shell pwd)/$(BUILD_DIR)" ../../../live555)
	( cd $(LIVE555_DIR) && cmake --build . && cmake --install . )

clean:
	rm -rf $(BUILD_DIR)
