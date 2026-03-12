DEVICE ?= 192.168.2.42
CC = arm-linux-gnueabihf-gcc
CFLAGS = -static -O2 -Wall -Wextra

.PHONY: all clean deploy

all:
	@mkdir -p build
	docker run --rm -v "$(PWD):/work" --platform linux/amd64 ubuntu:22.04 \
	  bash -c "apt-get update -qq && \
	           apt-get install -y -qq gcc-arm-linux-gnueabihf >/dev/null 2>&1 && \
	           $(CC) $(CFLAGS) -o /work/build/demo /work/demo.c /work/shelbourne.c -lm && \
	           $(CC) $(CFLAGS) -o /work/build/shelbourne-test /work/shelbourne-test.c /work/shelbourne.c -lm"
	@echo "Built: build/demo build/shelbourne-test"

deploy: all
	scp build/demo build/shelbourne-test root@$(DEVICE):/tmp/
	@echo "Deployed to $(DEVICE):/tmp/"

clean:
	rm -rf build
