/* SPDX-License-Identifier: GPL-2.0 */
static const char *__doc__ = "Simple XDP prog doing XDP_PASS\n";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <net/if.h>
#include <linux/if_link.h> /* depend on kernel-headers installed */

#include "../common/common_params.h"

#define DM_MAXLEN 64
#define SUF_MAXLEN 64

struct qname_lpm_key {
	__u32 prefixlen;
	char rev_suf[SUF_MAXLEN];
};


static const struct option_wrapper long_options[] = {
	{{"help",        no_argument,		NULL, 'h' },
	 "Show help", false},

	{{"dev",         required_argument,	NULL, 'd' },
	 "Operate on device <ifname>", "<ifname>", true},

	{{"skb-mode",    no_argument,		NULL, 'S' },
	 "Install XDP program in SKB (AKA generic) mode"},

	{{"native-mode", no_argument,		NULL, 'N' },
	 "Install XDP program in native mode"},

	{{"auto-mode",   no_argument,		NULL, 'A' },
	 "Auto-detect SKB or native mode"},

	{{"force",       no_argument,		NULL, 'F' },
	 "Force install, replacing existing program on interface"},

	{{"unload",      no_argument,		NULL, 'U' },
	 "Unload XDP program instead of loading"},

	{{0, 0, NULL,  0 }, NULL, false}
};

int load_bpf_object_file__simple(const char *filename, struct bpf_object** o_obj)
{
	int first_prog_fd = -1;
	struct bpf_object *obj;
	int err;

	/* Use libbpf for extracting BPF byte-code from BPF-ELF object, and
	 * loading this into the kernel via bpf-syscall
	 */
	err = bpf_prog_load(filename, BPF_PROG_TYPE_XDP, &obj, &first_prog_fd);
	if (err) {
		fprintf(stderr, "ERR: loading BPF-OBJ file(%s) (%d): %s\n",
			filename, err, strerror(-err));
		*o_obj = NULL;
        return -1;
	}

	/* Simply return the first program file descriptor.
	 * (Hint: This will get more advanced later)
	 */
    *o_obj = obj;
	return first_prog_fd;
}

static int xdp_link_detach(int ifindex, __u32 xdp_flags)
{
	/* Next assignment this will move into ../common/
	 * (in more generic version)
	 */
	int err;

	if ((err = bpf_set_link_xdp_fd(ifindex, -1, xdp_flags)) < 0) {
		fprintf(stderr, "ERR: link set xdp unload failed (err=%d):%s\n",
			err, strerror(-err));
		return EXIT_FAIL_XDP;
	}
	return EXIT_OK;
}

int xdp_link_attach(int ifindex, __u32 xdp_flags, int prog_fd)
{
	/* Next assignment this will move into ../common/ */
	int err;

	/* libbpf provide the XDP net_device link-level hook attach helper */
	err = bpf_set_link_xdp_fd(ifindex, prog_fd, xdp_flags);
	if (err == -EEXIST && !(xdp_flags & XDP_FLAGS_UPDATE_IF_NOEXIST)) {
		/* Force mode didn't work, probably because a program of the
		 * opposite type is loaded. Let's unload that and try loading
		 * again.
		 */

		__u32 old_flags = xdp_flags;

		xdp_flags &= ~XDP_FLAGS_MODES;
		xdp_flags |= (old_flags & XDP_FLAGS_SKB_MODE) ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
		err = bpf_set_link_xdp_fd(ifindex, -1, xdp_flags);
		if (!err)
			err = bpf_set_link_xdp_fd(ifindex, prog_fd, old_flags);
	}

	if (err < 0) {
		fprintf(stderr, "ERR: "
			"ifindex(%d) link set xdp fd failed (%d): %s\n",
			ifindex, -err, strerror(-err));

		switch (-err) {
		case EBUSY:
		case EEXIST:
			fprintf(stderr, "Hint: XDP already loaded on device"
				" use --force to swap/replace\n");
			break;
		case EOPNOTSUPP:
			fprintf(stderr, "Hint: Native-XDP not supported"
				" use --skb-mode or --auto-mode\n");
			break;
		default:
			break;
		}
		return EXIT_FAIL_XDP;
	}

	return EXIT_OK;
}

int strrev(char *src, int len){
	char * tmp = malloc(len);
	strncpy(tmp, src, len);
	for(int i=0;i<len;++i){
		src[i] = tmp[len-1-i];
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct bpf_prog_info info = {};
	__u32 info_len = sizeof(info);
	char filename[256] = "xdp_pass_kern.o";
	int prog_fd, err;

	struct config cfg = {
		.xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_DRV_MODE,
		.ifindex   = -1,
		.do_unload = false,
	};

	parse_cmdline_args(argc, argv, long_options, &cfg, __doc__);
	/* Required option */
	if (cfg.ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing\n");
		usage(argv[0], __doc__, long_options, (argc == 1));
		return EXIT_FAIL_OPTION;
	}
	if (cfg.do_unload)
		return xdp_link_detach(cfg.ifindex, cfg.xdp_flags);

    struct bpf_object *obj;

	/* Load the BPF-ELF object file and get back first BPF_prog FD */
	prog_fd = load_bpf_object_file__simple(filename, &obj);
	if (prog_fd <= 0) {
		fprintf(stderr, "ERR: loading file: %s\n", filename);
		return EXIT_FAIL_BPF;
	}

    struct bpf_map *dmap = NULL;
    if(!(dmap = bpf_object__find_map_by_name(obj, "dns_block_suffixes"))){
        fprintf(stderr, "ERR: loading map: %s\n", "dns_block_suffixes");
		return EXIT_FAIL_BPF;
    }
	int dmap_fd = bpf_map__fd(dmap);
    
	FILE *fp;
	char fname[256] = "../block_domains.t";
	if((fp = fopen(fname,"r")) == NULL){
	perror("fail to read");
	exit (1) ;
	}

	char buf[SUF_MAXLEN] = {0};
	__u32 len;
	int cnt = 0;
	struct qname_lpm_key qlk;
	while(fgets(buf,SUF_MAXLEN,fp) != NULL){
		len = strlen(buf);
		buf[len] = '\0';  /*去掉换行符*/
		printf("qname %s\n", buf);
		strrev(buf, len);
		printf("rev qname %s\n", buf);
		memset(qlk.rev_suf, 0, SUF_MAXLEN);
		qlk.prefixlen = len-1;
		strncpy(qlk.rev_suf, buf, len);
		bpf_map_update_elem(dmap_fd, &qlk, &cnt, BPF_ANY);
		memset(buf, 0, SUF_MAXLEN);
		cnt++;
	}

	/* At this point: BPF-prog is (only) loaded by the kernel, and prog_fd
	 * is our file-descriptor handle. Next step is attaching this FD to a
	 * kernel hook point, in this case XDP net_device link-level hook.
	 * Fortunately libbpf have a helper for this:
	 */
	err = xdp_link_attach(cfg.ifindex, cfg.xdp_flags, prog_fd);
	if (err)
		return err;

        /* This step is not really needed , BPF-info via bpf-syscall */
	err = bpf_obj_get_info_by_fd(prog_fd, &info, &info_len);
	if (err) {
		fprintf(stderr, "ERR: can't get prog info - %s\n",
			strerror(errno));
		return err;
	}

	printf("Success: Loading "
	       "XDP prog name:%s(id:%d) on device:%s(ifindex:%d)\n",
	       info.name, info.id, cfg.ifname, cfg.ifindex);
	return EXIT_OK;
}
