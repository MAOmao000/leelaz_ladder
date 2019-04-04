THE_OS := $(shell uname -s)

default:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=gcc CXX=g++ AR=ar \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -Wno-ignored-attributes -pipe -O3 -ffast-math -flto -march=native -mfpmath=sse -msse2 -std=c++14 -DNDEBUG -Wl,--stack,104857600'  \
		LDFLAGS='$(LDFLAGS) -flto' \
		libRay.a

debug:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=gcc CXX=g++ AR=ar \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -Wno-ignored-attributes -pipe -Og -g -std=c++14' \
		LDFLAGS='$(LDFLAGS) -g' \
		libRay.a

clang:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=clang CXX=clang++ AR=ar \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -Wno-missing-braces -Wno-mismatched-tags -O3 -ffast-math -flto -march=native -std=c++14 -DNDEBUG' \
		LDFLAGS='$(LDFLAGS) -flto -fuse-linker-plugin' \
		libRay.a

DYNAMIC_LIBS = -lboost_system -lboost_filesystem -lboost_program_options -lpthread -lz
LIBS =

ifeq ($(THE_OS),Linux)
# for Linux with OpenBLAS
	CXXFLAGS += -I/usr/include/openblas -I./Eigen
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
#DYNAMIC_LIBS += -lmkl_rt
#CXXFLAGS += -I/opt/intel/mkl/include
#LDFLAGS  += -L/opt/intel/mkl/lib/intel64/

CXXFLAGS += -I. -I../
CPPFLAGS += -MD -MP

sources = GoBoard.cpp Ladder.cpp Pattern.cpp SearchBoard.cpp ZobristHash.cpp

objects = $(sources:.cpp=.o)
deps = $(sources:%.cpp=%.d)

-include $(deps)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

libRay.a: $(objects)
	$(AR) r $@ $^ $(LIBS)

clean:
	-$(RM) libRay.a $(objects) $(deps)

.PHONY: clean default debug clang
