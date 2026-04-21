CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -pthread -DSERIAL_DATE=\"$$(date +%Y%m%d)\"
INCLUDES = -I./include -I/opt/homebrew/include
LIBS = -lpthread -L/opt/homebrew/lib -luv

SRCDIR = src
OBJDIR = obj
SOURCES = $(wildcard $(SRCDIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
DISTDIR = dist
TARGET = $(DISTDIR)/fnos-remote-ups

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(DISTDIR)
	$(CXX) $(OBJECTS) -o $@ $(LIBS)

$(DISTDIR):
	mkdir -p $(DISTDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET)

pack: $(TARGET)
	# Create a temporary directory for packaging
	TMP_DIR=$$(mktemp -d) && \
	PACKAGE_NAME=fnos-remote-ups && \
	mkdir -p $$TMP_DIR/$$PACKAGE_NAME && \
	mkdir -p dist && \
	cp $(TARGET) $$TMP_DIR/$$PACKAGE_NAME/ && \
	cp *.md $$TMP_DIR/$$PACKAGE_NAME/ 2>/dev/null || true && \
	cp *.sh $$TMP_DIR/$$PACKAGE_NAME/ 2>/dev/null || true && \
	cp *.service $$TMP_DIR/$$PACKAGE_NAME/ 2>/dev/null || true && \
	tar -czf dist/$$PACKAGE_NAME-$$(date +%Y%m%d).tar.gz -C $$TMP_DIR $$PACKAGE_NAME && \
	rm -rf $$TMP_DIR
	@echo "Package created successfully!"

docker:
	docker build -f ./docker/Dockerfile -t $(TARGET) . && \
	docker image save $(TARGET) | gzip > dist/$(TARGET).img.gz

.PHONY: all clean pack docker
