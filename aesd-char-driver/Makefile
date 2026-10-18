obj-m := aesdchar.o
aesdchar-y := aesd-circular-buffer.o main.o

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
