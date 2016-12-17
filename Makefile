ccflags-y += -I$(src)/include

udrm-y := udrm-dev.o udrm-drv.o udrm-fb.o udrm-pipe.o udrm-dmabuf.o
obj-$(CONFIG_DRM_USER) += udrm.o
