ccflags-y += -I$(src)/include

udrm-y := udrm-drv.o udrm-fb.o udrm-pipe.o
obj-$(CONFIG_DRM_USER) += udrm.o
