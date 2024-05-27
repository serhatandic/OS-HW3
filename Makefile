# Define the compiler
CXX = g++

# Define the compiler flags
CXXFLAGS = -Wall -g

# Define the source files
SRCS = recext2fs.cpp ext2fs_print.c identifier.cpp

# Define the header files
HDRS = ext2fs.h ext2fs_print.h identifier.h

# Define the output executable
TARGET = recext2fs

# Rule to build the target
all: $(TARGET)

# Rule to link the object files and create the executable
$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

# Rule to clean the build directory
clean:
	rm -f $(TARGET) *.o

# Phony targets
.PHONY: all clean
