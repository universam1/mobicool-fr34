# Environment 
MKDIR=mkdir
CP=cp
CCADMIN=CCadmin
RANLIB=ranlib

# build targets
build: .build-pre .build-post

.build-pre:
	mkdir -p dist/default/production
	xc8-cc -mcpu=16F1829 -O2 \
	-o dist/default/production/MobicoolFR34.X.production.hex \
	main.c analog.c irmcf183.c tm1620b.c settings.c display.c \
	mcc_generated_files/adc.c mcc_generated_files/device_config.c \
	mcc_generated_files/eusart.c mcc_generated_files/mcc.c \
	mcc_generated_files/memory.c mcc_generated_files/pin_manager.c \
	mcc_generated_files/tmr1.c modbus.c

.build-post:

# clean targets
clean: .clean-pre .clean-post

.clean-pre:
	rm -rf dist/default/production

.clean-post:

# clobber targets
clobber: clean

# all targets
all: build

# help target
help:
	echo "Available targets:"
	echo "  build    - Build the project"
	echo "  clean    - Remove build artifacts"
	echo "  clobber  - Remove all generated files"
	echo "  all      - Build everything"
	echo "  help     - Show this help message"
