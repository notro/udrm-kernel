/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/dma-buf.h>
#include <linux/slab.h>

struct udrm_dmabuf_object {
	struct device *dev;
	unsigned long attrs;
	dma_addr_t dma_addr;
	void *vaddr;
	size_t size;
};

static struct sg_table *udrm_dmabuf_map_dma_buf(struct dma_buf_attachment *attach,
					    enum dma_data_direction dir)
{
	struct udrm_dmabuf_object *obj = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(obj->dev, sgt, obj->vaddr,
			      obj->dma_addr, obj->size);
	if (ret < 0)
		goto err_free;

	if (!dma_map_sg(attach->dev, sgt->sgl, sgt->nents, dir)) {
		ret = -ENOMEM;
		goto err_free_table;
	}

	return sgt;

err_free_table:
	sg_free_table(sgt);
err_free:
	kfree(sgt);

	return ERR_PTR(ret);
}

static void udrm_dmabuf_unmap_dma_buf(struct dma_buf_attachment *attach,
				      struct sg_table *sgt,
				      enum dma_data_direction dir)
{
	dma_unmap_sg(attach->dev, sgt->sgl, sgt->nents, dir);
	sg_free_table(sgt);
	kfree(sgt);
}

static void udrm_dmabuf_release(struct dma_buf *dma_buf)
{
	struct udrm_dmabuf_object *obj = dma_buf->priv;

printk("%s()\n", __func__);
	dma_free_attrs(obj->dev, obj->size, obj->vaddr, obj->dma_addr,
		       obj->attrs);
	kfree(obj);
}

static void *udrm_dmabuf_kmap(struct dma_buf *dma_buf, unsigned long page_num)
{
	struct udrm_dmabuf_object *obj = dma_buf->priv;

	return obj->vaddr + page_num * PAGE_SIZE;;
}

static void *udrm_dmabuf_vmap(struct dma_buf *dma_buf)
{
	struct udrm_dmabuf_object *obj = dma_buf->priv;

	return obj->vaddr;
}

static int udrm_dmabuf_mmap(struct dma_buf *dma_buf,
			    struct vm_area_struct *vma)
{
	struct udrm_dmabuf_object *obj = dma_buf->priv;
	int ret;

	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;

	ret = dma_mmap_attrs(obj->dev, vma, obj->vaddr, obj->dma_addr,
			     vma->vm_end - vma->vm_start, obj->attrs);

	return ret;
}

static const struct dma_buf_ops udrm_dmabuf_ops =  {
	.map_dma_buf = udrm_dmabuf_map_dma_buf,
	.unmap_dma_buf = udrm_dmabuf_unmap_dma_buf,
	.release = udrm_dmabuf_release,
	.kmap_atomic = udrm_dmabuf_kmap,
	.kmap = udrm_dmabuf_kmap,
	.vmap = udrm_dmabuf_vmap,
	.mmap = udrm_dmabuf_mmap,
};

struct dma_buf *udrm_dmabuf_alloc_attrs(struct device *dev, size_t size,
					unsigned long attrs, int flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct udrm_dmabuf_object *obj;
	struct dma_buf *dmabuf;
	int ret;

	if (flags & ~(O_CLOEXEC | O_ACCMODE))
		return ERR_PTR(-EINVAL);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->dev = dev;
	obj->size = size;
	obj->attrs = attrs;

	obj->vaddr = dma_alloc_attrs(dev, size, &obj->dma_addr, GFP_KERNEL,
				     attrs);
	if (!obj->vaddr) {
		ret = -ENOMEM;
		goto err_free_obj;
	}

	exp_info.ops = &udrm_dmabuf_ops;
	exp_info.size = obj->size;
	exp_info.flags = flags;
	exp_info.priv = obj;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_free_buf;
	}

	return dmabuf;

err_free_buf:
	dma_free_attrs(dev, size, obj->vaddr, obj->dma_addr, attrs);
err_free_obj:
	kfree(obj);

	return ERR_PTR(ret);
}
