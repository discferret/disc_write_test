CXXFLAGS=-g -Ilibdiscferret/include -std=gnu++11
LDFLAGS=-Llibdiscferret/output -ldiscferret

.PHONY: all
all: writetool

writetool: main.o
	$(CXX) $(CXXFLAGS) -o$@ $^ $(LDFLAGS)

.PHONY: clean
clean:
	-rm -fr main.o writetool
