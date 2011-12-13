/* make -C /lib/modules/$(uname -r)/build modules M=$(pwd) */
/* make -C /lib/modules/$(uname -r)/build clean   M=$(pwd) */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <linux/version.h>
#include <net/icmp.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/route.h>
#include <net/dst.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter/x_tables.h>
#include "xt_MEDIAPROXY.h"

MODULE_LICENSE("GPL");




#define MAX_ID 64 /* - 1 */

#if 0
#define DBG(x...) printk(KERN_DEBUG x)
#else
#define DBG(x...) ((void)0)
#endif





static struct proc_dir_entry *my_proc_root;
static struct proc_dir_entry *proc_list;
static struct proc_dir_entry *proc_control;
static struct mediaproxy_table *table[64];
static rwlock_t table_lock;




static ssize_t proc_control_write(struct file *, const char __user *, size_t, loff_t *);
static int proc_control_open(struct inode *, struct file *);
static int proc_control_close(struct inode *, struct file *);
static int proc_status(char *, char **, off_t, int, int *, void *);

static ssize_t proc_main_control_write(struct file *, const char __user *, size_t, loff_t *);
static int proc_main_control_open(struct inode *, struct file *);
static int proc_main_control_close(struct inode *, struct file *);

static int proc_list_open(struct inode *, struct file *);

static void *proc_list_start(struct seq_file *, loff_t *);
static void proc_list_stop(struct seq_file *, void *);
static void *proc_list_next(struct seq_file *, void *, loff_t *);
static int proc_list_show(struct seq_file *, void *);

static int proc_blist_open(struct inode *, struct file *);
static int proc_blist_close(struct inode *, struct file *);
static ssize_t proc_blist_read(struct file *, char __user *, size_t, loff_t *);

static int proc_main_list_open(struct inode *, struct file *);

static void *proc_main_list_start(struct seq_file *, loff_t *);
static void proc_main_list_stop(struct seq_file *, void *);
static void *proc_main_list_next(struct seq_file *, void *, loff_t *);
static int proc_main_list_show(struct seq_file *, void *);

static void table_push(struct mediaproxy_table *);
static struct mediaproxy_target *get_target(struct mediaproxy_table *, u_int16_t);


static const struct file_operations proc_control_ops = {
	.write			= proc_control_write,
	.open			= proc_control_open,
	.release		= proc_control_close,
};

static const struct file_operations proc_main_control_ops = {
	.write			= proc_main_control_write,
	.open			= proc_main_control_open,
	.release		= proc_main_control_close,
};

static const struct file_operations proc_list_ops = {
	.open			= proc_list_open,
	.read			= seq_read,
	.llseek			= seq_lseek,
	.release		= seq_release,
};

static const struct file_operations proc_blist_ops = {
	.open			= proc_blist_open,
	.read			= proc_blist_read,
	.release		= proc_blist_close,
};

static struct seq_operations proc_list_seq_ops = {
	.start			= proc_list_start,
	.next			= proc_list_next,
	.stop			= proc_list_stop,
	.show			= proc_list_show,
};

static const struct file_operations proc_main_list_ops = {
	.open			= proc_main_list_open,
	.read			= seq_read,
	.llseek			= seq_lseek,
	.release		= seq_release,
};

static struct seq_operations proc_main_list_seq_ops = {
	.start			= proc_main_list_start,
	.next			= proc_main_list_next,
	.stop			= proc_main_list_stop,
	.show			= proc_main_list_show,
};





static struct mediaproxy_table *new_table(void) {
	struct mediaproxy_table *t;

	DBG(KERN_DEBUG "Creating new table\n");

	if (!try_module_get(THIS_MODULE))
		return NULL;

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		module_put(THIS_MODULE);
		return NULL;
	}

	memset(t, 0, sizeof(*t));

	atomic_set(&t->refcnt, 1);
	rwlock_init(&t->target_lock);
	t->id = -1;

	return t;
}




static void table_hold(struct mediaproxy_table *t) {
	atomic_inc(&t->refcnt);
}





static int table_create_proc(struct mediaproxy_table *t, u_int32_t id) {
	char num[10];

	sprintf(num, "%u", id);

	t->proc = create_proc_entry(num, S_IFDIR | S_IRUGO | S_IXUGO, my_proc_root);
	if (!t->proc)
		return -1;
	/* t->proc->owner = THIS_MODULE; */

	t->status = create_proc_entry("status", S_IFREG | S_IRUGO, t->proc);
	if (!t->status)
		return -1;
	/* t->status->owner = THIS_MODULE; */
	t->status->read_proc = proc_status;
	t->status->data = (void *) (unsigned long) id;

	t->control = create_proc_entry("control", S_IFREG | S_IWUSR | S_IWGRP, t->proc);
	if (!t->control)
		return -1;
	/* t->control->owner = THIS_MODULE; */
	t->control->proc_fops = &proc_control_ops;
	t->control->data = (void *) (unsigned long) id;

	t->list = create_proc_entry("list", S_IFREG | S_IRUGO, t->proc);
	if (!t->list)
		return -1;
	/* t->list->owner = THIS_MODULE; */
	t->list->proc_fops = &proc_list_ops;
	t->list->data = (void *) (unsigned long) id;

	t->blist = create_proc_entry("blist", S_IFREG | S_IRUGO, t->proc);
	if (!t->blist)
		return -1;
	/* t->blist->owner = THIS_MODULE; */
	t->blist->proc_fops = &proc_blist_ops;
	t->blist->data = (void *) (unsigned long) id;

	return 0;
}




static struct mediaproxy_table *new_table_link(u_int32_t id) {
	struct mediaproxy_table *t;
	unsigned long flags;

	if (id >= MAX_ID)
		return NULL;

	t = new_table();
	if (!t) {
		printk(KERN_WARNING "xt_MEDIAPROXY out of memory\n");
		return NULL;
	}

	write_lock_irqsave(&table_lock, flags);
	if (table[id]) {
		write_unlock_irqrestore(&table_lock, flags);
		table_push(t);
		printk(KERN_WARNING "xt_MEDIAPROXY duplicate ID %u\n", id);
		return NULL;
	}

	table_hold(t);
	table[id] = t;
	t->id = id;
	write_unlock_irqrestore(&table_lock, flags);

	if (table_create_proc(t, id))
		printk(KERN_WARNING "xt_MEDIAPROXY failed to create /proc entry for ID %u\n", id);


	return t;
}





static void target_push(struct mediaproxy_target *t) {
	if (!t)
		return;

	if (!atomic_dec_and_test(&t->refcnt))
		return;

	DBG(KERN_DEBUG "Freeing target\n");

	kfree(t);
}






static void target_hold(struct mediaproxy_target *t) {
	atomic_inc(&t->refcnt);
}






static void clear_proc(struct proc_dir_entry **e) {
	if (!e || !*e)
		return;

	remove_proc_entry((*e)->name, (*e)->parent);
	*e = NULL;
}





static void table_push(struct mediaproxy_table *t) {
	int i, j;

	if (!t)
		return;

	if (!atomic_dec_and_test(&t->refcnt))
		return;

	DBG(KERN_DEBUG "Freeing table\n");

	for (i = 0; i < 256; i++) {
		if (!t->target[i])
			continue;

		for (j = 0; j < 256; j++) {
			if (!t->target[i][j])
				continue;
			t->target[i][j]->table = -1;
			target_push(t->target[i][j]);
			t->target[i][j] = NULL;
		}

		kfree(t->target[i]);
		t->target[i] = NULL;
	}

	clear_proc(&t->status);
	clear_proc(&t->control);
	clear_proc(&t->list);
	clear_proc(&t->blist);
	clear_proc(&t->proc);

	kfree(t);

	module_put(THIS_MODULE);
}




static int unlink_table(struct mediaproxy_table *t) {
	unsigned long flags;

	if (t->id >= MAX_ID)
		return -EINVAL;

	DBG(KERN_DEBUG "Unlinking table %u\n", t->id);

	write_lock_irqsave(&table_lock, flags);
	if (t->id >= MAX_ID || table[t->id] != t) {
		write_unlock_irqrestore(&table_lock, flags);
		return -EINVAL;
	}
	if (t->pid) {
		write_unlock_irqrestore(&table_lock, flags);
		return -EBUSY;
	}
	table[t->id] = NULL;
	t->id = -1;
	write_unlock_irqrestore(&table_lock, flags);

	clear_proc(&t->status);
	clear_proc(&t->control);
	clear_proc(&t->list);
	clear_proc(&t->blist);
	clear_proc(&t->proc);

	table_push(t);

	return 0;
}




static struct mediaproxy_table *get_table(u_int32_t id) {
	struct mediaproxy_table *t;
	unsigned long flags;

	if (id >= MAX_ID)
		return NULL;

	read_lock_irqsave(&table_lock, flags);
	t = table[id];
	if (t)
		table_hold(t);
	read_unlock_irqrestore(&table_lock, flags);

	return t;
}




static int proc_status(char *page, char **start, off_t off, int count, int *eof, void *data) {
	struct mediaproxy_table *t;
	int len = 0;
	unsigned long flags;

	u_int32_t id = (u_int32_t) (unsigned long) data;
	t = get_table(id);
	if (!t)
		return -ENOENT;

	read_lock_irqsave(&t->target_lock, flags);
	len += sprintf(page + len, "Refcount:    %u\n", atomic_read(&t->refcnt) - 1);
	len += sprintf(page + len, "Control PID: %u\n", t->pid);
	len += sprintf(page + len, "Targets:     %u\n", t->targets);
	len += sprintf(page + len, "Buckets:     %u\n", t->buckets);
	read_unlock_irqrestore(&t->target_lock, flags);

	table_push(t);

	return len;
}




static int proc_main_list_open(struct inode *i, struct file *f) {
	return seq_open(f, &proc_main_list_seq_ops);
}





static void *proc_main_list_start(struct seq_file *f, loff_t *o) {
	if (!try_module_get(THIS_MODULE))
		return NULL;
	return proc_main_list_next(f, NULL, o);
}

static void proc_main_list_stop(struct seq_file *f, void *v) {
	module_put(THIS_MODULE);
}

static void *proc_main_list_next(struct seq_file *f, void *v, loff_t *o) {	/* v is invalid */
	struct mediaproxy_table *t = NULL;
	u_int32_t id;

	if (*o < 0)
		return NULL;
	id = *o;

	while (id < MAX_ID) {
		t = get_table(id++);
		if (!t)
			continue;
		break;
	}

	*o = id;

	return t;	/* might be NULL */
}

static int proc_main_list_show(struct seq_file *f, void *v) {
	struct mediaproxy_table *g = v;

	seq_printf(f, "%u\n", g->id);
	table_push(g);

	return 0;
}





static int proc_blist_open(struct inode *i, struct file *f) {
	struct proc_dir_entry *pde;
	u_int32_t id;
	struct mediaproxy_table *t;

	pde = PDE(i);
	id = (u_int32_t) (unsigned long) pde->data;
	t = get_table(id);
	if (!t)
		return -ENOENT;

	table_push(t);

	return 0;
}

static int proc_blist_close(struct inode *i, struct file *f) {
	struct proc_dir_entry *pde;
	u_int32_t id;
	struct mediaproxy_table *t;

	pde = PDE(i);
	id = (u_int32_t) (unsigned long) pde->data;
	t = get_table(id);
	if (!t)
		return 0;

	table_push(t);

	return 0;
}

static ssize_t proc_blist_read(struct file *f, char __user *b, size_t l, loff_t *o) {
	struct inode *inode;
	struct proc_dir_entry *pde;
	u_int32_t id;
	struct mediaproxy_table *t;
	struct mediaproxy_list_entry op;
	int err;
	struct mediaproxy_target *g;
	unsigned long flags;

	if (l != sizeof(op))
		return -EINVAL;
	if (*o < 0)
		return -EINVAL;

	inode = f->f_path.dentry->d_inode;
	pde = PDE(inode);
	id = (u_int32_t) (unsigned long) pde->data;
	t = get_table(id);
	if (!t)
		return -ENOENT;

	for (;;) {
		err = 0;
		if (*o > 0xffff)
			goto err;

		g = get_target(t, (*o)++);
		if (g)
			break;
	}

	memset(&op, 0, sizeof(op));
	spin_lock_irqsave(&g->lock, flags);
	memcpy(&op.target, &g->target, sizeof(op.target));
	memcpy(&op.stats, &g->stats, sizeof(op.stats));
	spin_unlock_irqrestore(&g->lock, flags);

	target_push(g);

	err = -EFAULT;
	if (copy_to_user(b, &op, sizeof(op)))
		goto err;

	table_push(t);
	return l;

err:
	table_push(t);
	return err;
}





static int proc_list_open(struct inode *i, struct file *f) {
	int err;
	struct seq_file *p;
	struct proc_dir_entry *pde;
	u_int32_t id;
	struct mediaproxy_table *t;

	pde = PDE(i);
	id = (u_int32_t) (unsigned long) pde->data;
	t = get_table(id);
	if (!t)
		return -ENOENT;
	table_push(t);

	err = seq_open(f, &proc_list_seq_ops);
	if (err)
		return err;

	p = f->private_data;
	p->private = (void *) (unsigned long) id;

	return 0;
}




static void *proc_list_start(struct seq_file *f, loff_t *o) {
	return proc_list_next(f, NULL, o);
}

static void proc_list_stop(struct seq_file *f, void *v) {
}

static void *proc_list_next(struct seq_file *f, void *v, loff_t *o) {	/* v is invalid */
	u_int32_t id = (u_int32_t) (unsigned long) f->private;
	struct mediaproxy_target *g = NULL;
	struct mediaproxy_table *t;
	u_int16_t port;
	unsigned char hi, lo;
	unsigned long flags;

	if (*o < 0 || *o > 0xffff)
		return NULL;
	port = (u_int16_t) *o;

	t = get_table(id);
	if (!t)
		return NULL;

	hi = (port & 0xff00) >> 8;
	lo = port & 0xff;

	read_lock_irqsave(&t->target_lock, flags);
	for (;;) {
		lo++;	/* will make the iteration start from 1 */
		if (lo == 0) {
			hi++;
			if (hi == 0)
				break;
		}
		if (!t->target[hi]) {
			lo = 0xff;
			continue;
		}

		g = t->target[hi][lo];
		if (!g)
			continue;

		target_hold(g);
		break;
	}
	read_unlock_irqrestore(&t->target_lock, flags);

	*o = (hi << 8) | lo;
	table_push(t);

	return g;
}

static int proc_list_show(struct seq_file *f, void *v) {
	struct mediaproxy_target *g = v;
	unsigned long flags;

	spin_lock_irqsave(&g->lock, flags);
	seq_printf(f, "port %5u: %20llu bytes, %20llu packets, %20llu errors\n",
		g->target.target_port, g->stats.bytes, g->stats.packets, g->stats.errors);
	spin_unlock_irqrestore(&g->lock, flags);

	target_push(g);

	return 0;
}





static int table_del_target(struct mediaproxy_table *t, u_int16_t port) {
	unsigned char hi, lo;
	struct mediaproxy_target *g;
	unsigned long flags;

	if (!port)
		return -EINVAL;

	hi = (port & 0xff00) >> 8;
	lo = port & 0xff;

	write_lock_irqsave(&t->target_lock, flags);
	g = t->target[hi] ? t->target[hi][lo] : NULL;
	if (g) {
		t->target[hi][lo] = NULL;
		t->targets--;
	}
	write_unlock_irqrestore(&t->target_lock, flags);

	if (!g)
		return -ENOENT;

	target_push(g);

	return 0;
}





static int table_new_target(struct mediaproxy_table *t, struct mediaproxy_target_info *i, int update) {
	unsigned char hi, lo;
	struct mediaproxy_target *g;
	struct mediaproxy_target **gp;
	struct mediaproxy_target *og = NULL;
	int err;
	unsigned long flags;

	if (!i->target_port)
		return -EINVAL;
	if (!i->src_ip)
		return -EINVAL;
	if (!i->dst_ip)
		return -EINVAL;
	if (!i->src_port)
		return -EINVAL;
	if (!i->dst_port)
		return -EINVAL;

	DBG(KERN_DEBUG "Creating new target\n");

	err = -ENOMEM;
	g = kmalloc(sizeof(*g), GFP_KERNEL);
	if (!g)
		goto fail1;
	memset(g, 0, sizeof(*g));
	g->table = t->id;
	atomic_set(&g->refcnt, 1);
	spin_lock_init(&g->lock);
	memcpy(&g->target, i, sizeof(*i));

	if (update)
		gp = NULL;
	else {
		gp = kmalloc(sizeof(void *) * 256, GFP_KERNEL);
		if (!gp)
			goto fail2;
		memset(gp, 0, sizeof(void *) * 256);
	}

	hi = (i->target_port & 0xff00) >> 8;
	lo = i->target_port & 0xff;

	write_lock_irqsave(&t->target_lock, flags);
	if (!t->target[hi]) {
		err = -ENOENT;
		if (update)
			goto fail4;
		t->target[hi] = gp;
		gp = NULL;
		t->buckets++;
	}
	if (update) {
		err = -ENOENT;
		og = t->target[hi][lo];
		if (!og)
			goto fail4;

		spin_lock(&og->lock);	/* nested lock! irqs are disabled already */
		memcpy(&g->stats, &og->stats, sizeof(g->stats));
		spin_unlock(&og->lock);
	}
	else {
		err = -EEXIST;
		if (t->target[hi][lo])
			goto fail4;
	}

	t->target[hi][lo] = g;
	g = NULL;
	if (!update)
		t->targets++;
	write_unlock_irqrestore(&t->target_lock, flags);

	if (gp)
		kfree(gp);
	if (og)
		target_push(og);

	return 0;

fail4:
	write_unlock_irqrestore(&t->target_lock, flags);
	if (gp)
		kfree(gp);
fail2:
	kfree(g);
fail1:
	return err;
}





static struct mediaproxy_target *get_target(struct mediaproxy_table *t, u_int16_t port) {
	unsigned char hi, lo;
	struct mediaproxy_target *r;
	unsigned long flags;

	if (!t)
		return NULL;
	if (!port)
		return NULL;

	hi = (port & 0xff00) >> 8;
	lo = port & 0xff;

	read_lock_irqsave(&t->target_lock, flags);
	r = t->target[hi] ? t->target[hi][lo] : NULL;
	if (r)
		target_hold(r);
	read_unlock_irqrestore(&t->target_lock, flags);

	return r;
}





static int proc_main_control_open(struct inode *inode, struct file *file) {
	if (!try_module_get(THIS_MODULE))
		return -ENXIO;
	return 0;
}

static int proc_main_control_close(struct inode *inode, struct file *file) {
	module_put(THIS_MODULE);
	return 0;
}

static ssize_t proc_main_control_write(struct file *file, const char __user *buf, size_t buflen, loff_t *off) {
	char b[30];
	unsigned long id;
	char *endp;
	struct mediaproxy_table *t;
	int err;

	if (buflen < 6 || buflen > 20)
		return -EINVAL;

	if (copy_from_user(&b, buf, buflen))
		return -EFAULT;

	if (!strncmp(b, "add ", 4)) {
		id = simple_strtoul(b + 4, &endp, 10);
		if (endp == b + 4)
			return -EINVAL;
		if (id >= MAX_ID)
			return -EINVAL;
		t = new_table_link((u_int32_t) id);
		if (!t)
			return -EEXIST;
		table_push(t);
		t = NULL;
	}
	else if (!strncmp(b, "del ", 4)) {
		id = simple_strtoul(b + 4, &endp, 10);
		if (endp == b + 4)
			return -EINVAL;
		if (id >= MAX_ID)
			return -EINVAL;
		t = get_table((u_int32_t) id);
		if (!t)
			return -ENOENT;
		err = unlink_table(t);
		table_push(t);
		t = NULL;
		if (err)
			return err;
	}
	else
		return -EINVAL;

	return buflen;
}





static int proc_control_open(struct inode *inode, struct file *file) {
	struct proc_dir_entry *pde;
	u_int32_t id;
	struct mediaproxy_table *t;
	unsigned long flags;

	pde = PDE(inode);
	id = (u_int32_t) (unsigned long) pde->data;
	t = get_table(id);
	if (!t)
		return -ENOENT;

	write_lock_irqsave(&table_lock, flags);
	if (t->pid) {
		write_unlock_irqrestore(&table_lock, flags);
		table_push(t);
		return -EBUSY;
	}
	t->pid = current->tgid;
	write_unlock_irqrestore(&table_lock, flags);

	table_push(t);
	return 0;
}

static int proc_control_close(struct inode *inode, struct file *file) {
	struct proc_dir_entry *pde;
	u_int32_t id;
	struct mediaproxy_table *t;
	unsigned long flags;

	pde = PDE(inode);
	id = (u_int32_t) (unsigned long) pde->data;
	t = get_table(id);
	if (!t)
		return 0;

	write_lock_irqsave(&table_lock, flags);
	t->pid = 0;
	write_unlock_irqrestore(&table_lock, flags);

	table_push(t);

	return 0;
}

static ssize_t proc_control_write(struct file *file, const char __user *buf, size_t buflen, loff_t *off) {
	struct inode *inode;
	struct proc_dir_entry *pde;
	u_int32_t id;
	struct mediaproxy_table *t;
	struct mediaproxy_message msg;
	int err;

	if (buflen != sizeof(msg))
		return -EINVAL;

	inode = file->f_path.dentry->d_inode;
	pde = PDE(inode);
	id = (u_int32_t) (unsigned long) pde->data;
	t = get_table(id);
	if (!t)
		return -ENOENT;

	err = -EFAULT;
	if (copy_from_user(&msg, buf, sizeof(msg)))
		goto err;

	switch (msg.cmd) {
		case MMG_NOOP:
			DBG(KERN_DEBUG "noop.\n");
			break;

		case MMG_ADD:
			err = table_new_target(t, &msg.target, 0);
			if (err)
				goto err;
			break;

		case MMG_DEL:
			err = table_del_target(t, msg.target.target_port);
			if (err)
				goto err;
			break;

		case MMG_UPDATE:
			err = table_new_target(t, &msg.target, 1);
			if (err)
				goto err;
			break;

		default:
			printk(KERN_WARNING "xt_MEDIAPROXY unimplemented op %u\n", msg.cmd);
			err = -EINVAL;
			goto err;
	}

	table_push(t);

	return buflen;

err:
	table_push(t);
	return err;
}





static int send_proxy_packet(struct sk_buff *skb, u_int32_t sip, u_int16_t sport, u_int32_t dip, u_int16_t dport, unsigned char tos) {
	struct iphdr *ih;
	struct udphdr *uh;
	int datalen;

	ih = ip_hdr(skb);
	uh = udp_hdr(skb);

	datalen = ntohs(uh->len);

	ih->saddr = sip;
	ih->daddr = dip;
	ih->tos = tos;
	uh->source = htons(sport);
	uh->dest = htons(dport);

	uh->check = 0;
	skb->csum_start = skb_transport_header(skb) - skb->head;
	skb->csum_offset = offsetof(struct udphdr, check);
	uh->check = csum_tcpudp_magic(sip, dip, datalen, IPPROTO_UDP, csum_partial(uh, datalen, 0));
	if (uh->check == 0)
		uh->check = CSUM_MANGLED_0;

	__ip_select_ident(ih, skb_dst(skb), 0);

	if (ip_route_me_harder(skb, RTN_LOCAL))
		goto drop;

	skb->ip_summed = CHECKSUM_NONE;
	__ip_select_ident(ih, skb_dst(skb), 0);

	ip_local_out(skb);

	return 0;

drop:
	kfree_skb(skb);
	return -1;
}






#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
static unsigned int mediaproxy(struct sk_buff *oskb, const struct xt_target_param *par) {
#else
static unsigned int mediaproxy(struct sk_buff *oskb, const struct xt_action_param *par) {
#endif
	const struct xt_mediaproxy_info *pinfo = par->targinfo;
	struct sk_buff *skb;
	struct sk_buff *skb2;
	struct iphdr *ih;
	struct udphdr *uh;
	struct mediaproxy_table *t;
	struct mediaproxy_target *g;
	int err;
	unsigned long flags;

	t = get_table(pinfo->id);
	if (!t)
		goto skip;

	skb = skb_copy(oskb, GFP_ATOMIC);
	if (!skb)
		goto skip3;

	skb_reset_network_header(skb);
	ih = ip_hdr(skb);
	if (ih->protocol != IPPROTO_UDP)
		goto skip2;

	skb_set_transport_header(skb, (ih->ihl << 2));
	uh = udp_hdr(skb);

	g = get_target(t, ntohs(uh->dest));
	if (!g)
		goto skip2;

	DBG(KERN_DEBUG "target found, src %08x -> dst %08x\n", g->target.src_ip, g->target.dst_ip);

	if (g->target.mirror_ip && g->target.mirror_port) {
		DBG(KERN_DEBUG "sending mirror packet to dst %08x\n", g->target.mirror_ip);
		skb2 = skb_copy(skb, GFP_ATOMIC);
		err = send_proxy_packet(skb2, g->target.src_ip, g->target.src_port, g->target.mirror_ip, g->target.mirror_port, g->target.tos);
		if (err) {
			spin_lock_irqsave(&g->lock, flags);
			g->stats.errors++;
			spin_unlock_irqrestore(&g->lock, flags);
		}
	}

	err = send_proxy_packet(skb, g->target.src_ip, g->target.src_port, g->target.dst_ip, g->target.dst_port, g->target.tos);

	spin_lock_irqsave(&g->lock, flags);
	if (err)
		g->stats.errors++;
	else {
		g->stats.packets++;
		g->stats.bytes += skb->len;
	}
	spin_unlock_irqrestore(&g->lock, flags);

	target_push(g);
	table_push(t);

	return NF_DROP;

skip2:
	kfree_skb(skb);
skip3:
	table_push(t);
skip:
	return XT_CONTINUE;
}




#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
static unsigned int mediaproxy6(struct sk_buff *oskb, const struct xt_target_param *par) {
#else
static unsigned int mediaproxy6(struct sk_buff *oskb, const struct xt_action_param *par) {
#endif
	return 0;
}





#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
#define CHECK_ERR false
#define CHECK_SCC true
static bool check(const struct xt_tgchk_param *par) {
#else
#define CHECK_ERR -EINVAL
#define CHECK_SCC 0
static int check(const struct xt_tgchk_param *par) {
#endif
	const struct xt_mediaproxy_info *pinfo = par->targinfo;

	if (!my_proc_root) {
		printk(KERN_WARNING "xt_MEDIAPROXY check() without proc_root\n");
		return CHECK_ERR;
	}
	if (pinfo->id >= MAX_ID) {
		printk(KERN_WARNING "xt_MEDIAPROXY ID too high (%u >= %u)\n", pinfo->id, MAX_ID);
		return CHECK_ERR;
	}

	return CHECK_SCC;
}




static struct xt_target xt_mediaproxy_regs[] = {
	{
		.name		= "MEDIAPROXY",
		.family		= NFPROTO_IPV4,
		.target		= mediaproxy,
		.targetsize	= sizeof(struct xt_mediaproxy_info),
		.table		= "filter",
		.hooks		= (1 << NF_INET_LOCAL_IN),
		.checkentry	= check,
		.me		= THIS_MODULE,
	},
	{
		.name		= "MEDIAPROXY",
		.family		= NFPROTO_IPV6,
		.target		= mediaproxy6,
		.targetsize	= sizeof(struct xt_mediaproxy_info),
		.table		= "filter",
		.hooks		= (1 << NF_INET_LOCAL_IN),
		.checkentry	= check,
		.me		= THIS_MODULE,
	},
};

static int __init init(void) {
	int ret;

	printk(KERN_NOTICE "Registering xt_MEDIAPROXY module\n");

	rwlock_init(&table_lock);

	ret = -ENOMEM;
	my_proc_root = proc_mkdir("mediaproxy", NULL);
	if (!my_proc_root)
		goto fail;
	/* my_proc_root->owner = THIS_MODULE; */

	proc_control = create_proc_entry("control", S_IFREG | S_IWUSR | S_IWGRP, my_proc_root);
	if (!proc_control)
		goto fail;
	/* proc_control->owner = THIS_MODULE; */
	proc_control->proc_fops = &proc_main_control_ops;

	proc_list = create_proc_entry("list", S_IFREG | S_IRUGO, my_proc_root);
	if (!proc_list)
		goto fail;
	/* proc_list->owner = THIS_MODULE; */
	proc_list->proc_fops = &proc_main_list_ops;

	ret = xt_register_targets(xt_mediaproxy_regs, ARRAY_SIZE(xt_mediaproxy_regs));
	if (ret)
		goto fail;

	return 0;

fail:
	clear_proc(&proc_control);
	clear_proc(&proc_list);
	clear_proc(&my_proc_root);

	return ret;
}

static void __exit fini(void) {
	printk(KERN_NOTICE "Unregistering xt_MEDIAPROXY module\n");
	xt_unregister_targets(xt_mediaproxy_regs, ARRAY_SIZE(xt_mediaproxy_regs));

	clear_proc(&proc_control);
	clear_proc(&proc_list);
	clear_proc(&my_proc_root);
}

module_init(init);
module_exit(fini);