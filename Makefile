IMAGE  = shelbourne-build

CC      = arm-linux-gnueabihf-gcc
CFLAGS  = -static -O2 -Wall -Wextra

.PHONY: all clean deploy docker

docker:
	@docker inspect $(IMAGE) >/dev/null 2>&1 || \
	  docker build --platform linux/amd64 -t $(IMAGE) .

all: docker
	@mkdir -p build
	docker run --rm -v "$(PWD):/work" --platform linux/amd64 $(IMAGE) \
	  bash -c "$(CC) $(CFLAGS) -o /work/build/demo /work/demo.c /work/shelbourne.c -lm && \
	           $(CC) $(CFLAGS) -o /work/build/shelbourne-test /work/shelbourne-test.c /work/shelbourne.c -lm"
	@echo "Built: build/demo build/shelbourne-test"

deploy: all
ifndef DEVICE
	$(error DEVICE not set. Usage: make deploy DEVICE=<device-ip>)
endif
	scp build/demo build/shelbourne-test root@$(DEVICE):/tmp/
	@echo "Deployed to $(DEVICE):/tmp/"

clean:
	rm -rf build
