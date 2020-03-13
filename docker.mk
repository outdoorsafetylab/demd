
IMAGE_NAME := outdoorsafetylab/demd
REPO_NAME ?= outdoorsafetylab/demd
VERSION ?= $(subst v,,$(shell git describe --tags --exact-match 2>/dev/null || echo ""))
PORT ?= 8082
DEMS ?= $(realpath dem)

# Build docker image.
#
# Usage:
#	make docker/build [no-cache=(no|yes)]

docker/build:
	docker build --network=host --force-rm \
		$(if $(call eq,$(no-cache),yes),--no-cache --pull,) \
		--build-arg PORT=$(PORT) \
		-t $(IMAGE_NAME) .

# Run docker image.
#
# Usage:
#	make docker/run

docker/run: $(HGT)
	docker run -it --rm \
		-p $(PORT):$(PORT) \
		-v "$(DEMS):/var/lib/dem" \
		$(IMAGE_NAME)

# Tag docker images.
#
# Usage:
#	make docker/tag [VERSION=<image-version>]

docker/tag:
	docker tag $(IMAGE_NAME) $(REPO_NAME):latest
ifneq ($(VERSION),)
	docker tag $(IMAGE_NAME) $(REPO_NAME):$(VERSION)
endif

# Push docker images.
#
# Usage:
#	make docker/push

docker/push:
	docker push $(REPO_NAME):latest
ifneq ($(VERSION),)
	docker push $(REPO_NAME):$(VERSION)
endif

.PHONY: docker/build docker/run docker/tag docker/push
