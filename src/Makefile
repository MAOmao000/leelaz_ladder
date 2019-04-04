THE_OS := $(shell uname -s)

default:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=gcc CXX=g++ \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -Wno-ignored-attributes -pipe -g -Ofast -ffast-math -flto -fopenmp -mtune=native -march=native -mfpmath=both -std=c++14 -DNDEBUG -DUSE_CPU_ONLY -DUSE_BLAS' \
		LDFLAGS='$(LDFLAGS) -g -flto -fopenmp' \
		leelaz

debug:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=gcc CXX=g++ \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -Wno-ignored-attributes -pipe -Og -g -std=c++14' \
		LDFLAGS='$(LDFLAGS) -g' \
		leelaz

clang:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=clang CXX=clang++ \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -Wno-missing-braces -Wno-mismatched-tags -O3 -ffast-math -flto -march=native -std=c++14 -DNDEBUG' \
		LDFLAGS='$(LDFLAGS) -flto -fuse-linker-plugin' \
		leelaz

DYNAMIC_LIBS = -lboost_system -lboost_filesystem -lboost_program_options -lpthread -lz
LIBS =

ifeq ($(THE_OS),Linux)
# for Linux with OpenBLAS
	CXXFLAGS += -I/usr/include/openblas
	DYNAMIC_LIBS += -lopenblas
	DYNAMIC_LIBS += -lOpenCL
endif
ifeq ($(THE_OS),Darwin)
# for macOS (comment out the Linux part)
	LIBS += -framework Accelerate
	LIBS += -framework OpenCL
	CXXFLAGS += -I./Eigen
	CXXFLAGS += -I/System/Library/Frameworks/Accelerate.framework/Versions/Current/Headers
endif

# for MKL instead of OpenBLAS
#MKLROOT = /opt/intel/mkl/lib/intel64_lin
#DYNAMIC_LIBS += -Wl,--start-group ${MKLROOT}/libmkl_intel_ilp64.a ${MKLROOT}/libmkl_intel_thread.a ${MKLROOT}/libmkl_core.a ${MKLROOT}/libmkl_intel_lp64.a ${MKLROOT}/libmkl_sequential.a -Wl,--end-group -lgomp -lm -ldl -lc
#DYNAMIC_LIBS += -static-libgcc -Wl,-Bstatic -L/opt/intel/mkl/lib/intel_ilp64_lin/ -lmkl_core -lmkl_sequential -L/usr/lib/x86_64-linux-gnu/ -lc
#DYNAMIC_LIBS += -lmkl_rt
#CXXFLAGS += -I/opt/intel/mkl/include
#LDFLAGS  += -L/opt/intel/mkl/lib/intel64_lin/

CXXFLAGS += -I.
CXXFLAGS += -I./Eigen
CXXFLAGS += -I../
CPPFLAGS += -MD -MP

sources = Network.cpp FullBoard.cpp KoState.cpp Training.cpp \
	  TimeControl.cpp UCTSearch.cpp GameState.cpp Leela.cpp \
	  SGFParser.cpp Timing.cpp Utils.cpp FastBoard.cpp \
	  SGFTree.cpp Zobrist.cpp FastState.cpp GTP.cpp Random.cpp \
	  SMP.cpp UCTNode.cpp UCTNodePointer.cpp UCTNodeRoot.cpp \
	  OpenCL.cpp OpenCLScheduler.cpp NNCache.cpp Tuner.cpp CPUPipe.cpp

objects = $(sources:.cpp=.o)
deps = $(sources:%.cpp=%.d)

-include $(deps)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

leelaz: $(objects)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS) $(DYNAMIC_LIBS) -L../Ray -lRay

clean:
	-$(RM) leelaz $(objects) $(deps)

.PHONY: clean default debug clang
