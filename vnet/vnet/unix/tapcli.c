/* 
 *------------------------------------------------------------------
 * tapcli.c - dynamic tap interface hookup
 *
 * Copyright (c) 2009 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#include <fcntl.h>		/* for open */
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h> 
#include <sys/uio.h>		/* for iovec */
#include <netinet/in.h>

#include <linux/if_arp.h>
#include <linux/if_tun.h>

#include <vlib/vlib.h>
#include <vlib/unix/unix.h>

#include <vnet/ip/ip.h>

#include <vnet/ethernet/ethernet.h>

#if DPDK == 1
#include <vnet/devices/dpdk/dpdk.h>
#endif

#include <vnet/unix/tapcli.h>

static vnet_device_class_t tapcli_dev_class;
static vnet_hw_interface_class_t tapcli_interface_class;
static vlib_node_registration_t tapcli_rx_node;

static void tapcli_nopunt_frame (vlib_main_t * vm,
                                 vlib_node_runtime_t * node,
                                 vlib_frame_t * frame);
typedef struct {
  u32 unix_fd;
  u32 unix_file_index;
  u32 provision_fd;
  u32 sw_if_index;              /* for counters */
  u32 hw_if_index;
  u32 is_promisc;
  struct ifreq ifr;
  u32 per_interface_next_index;
  u8 active;                    /* for delete */
} tapcli_interface_t;

typedef struct {
  u16 sw_if_index;
} tapcli_rx_trace_t;

u8 * format_tapcli_rx_trace (u8 * s, va_list * va)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*va, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*va, vlib_node_t *);
  vnet_main_t * vnm = vnet_get_main();
  tapcli_rx_trace_t * t = va_arg (*va, tapcli_rx_trace_t *);
  s = format (s, "%U", format_vnet_sw_if_index_name,
                vnm, t->sw_if_index);
  return s;
}

typedef struct {
  /* Vector of iovecs for readv/writev calls. */
  struct iovec * iovecs;

  /* Vector of VLIB rx buffers to use.  We allocate them in blocks
     of VLIB_FRAME_SIZE (256). */
  u32 * rx_buffers;

  /* tap device destination MAC address. Required, or Linux drops pkts */
  u8 ether_dst_mac[6];

  /* Interface MTU in bytes and # of default sized buffers. */
  u32 mtu_bytes, mtu_buffers;

  /* Vector of tap interfaces */
  tapcli_interface_t * tapcli_interfaces;

  /* Vector of deleted tap interfaces */
  u32 * tapcli_inactive_interfaces;

  /* Bitmap of tap interfaces with pending reads */
  uword * pending_read_bitmap;

  /* Hash table to find tapcli interface given hw_if_index */
  uword * tapcli_interface_index_by_sw_if_index;
  
  /* Hash table to find tapcli interface given unix fd */
  uword * tapcli_interface_index_by_unix_fd;

  /* renumbering table */
  u32 * show_dev_instance_by_real_dev_instance;

  /* 1 => disable CLI */
  int is_disabled;

  /* convenience */
  vlib_main_t * vlib_main;
  vnet_main_t * vnet_main;
  unix_main_t * unix_main;
} tapcli_main_t;

static tapcli_main_t tapcli_main;

/*
 * tapcli_tx
 * Output node, writes the buffers comprising the incoming frame 
 * to the tun/tap device, aka hands them to the Linux kernel stack.
 * 
 */
static uword
tapcli_tx (vlib_main_t * vm,
	   vlib_node_runtime_t * node,
	   vlib_frame_t * frame)
{
  u32 * buffers = vlib_frame_args (frame);
  uword n_packets = frame->n_vectors;
  tapcli_main_t * tm = &tapcli_main;
  tapcli_interface_t * ti;
  int i;

  for (i = 0; i < n_packets; i++)
    {
      struct iovec * iov;
      vlib_buffer_t * b;
      uword l;
      vnet_hw_interface_t * hw;
      uword * p;
      u32 tx_sw_if_index;

      b = vlib_get_buffer (vm, buffers[i]);

      tx_sw_if_index = vnet_buffer(b)->sw_if_index[VLIB_TX];
      if (tx_sw_if_index == (u32)~0)
        tx_sw_if_index = vnet_buffer(b)->sw_if_index[VLIB_RX];
        
      ASSERT(tx_sw_if_index != (u32)~0);

      /* Use the sup intfc to finesse vlan subifs */
      hw = vnet_get_sup_hw_interface (tm->vnet_main, tx_sw_if_index);
      tx_sw_if_index = hw->sw_if_index;

      p = hash_get (tm->tapcli_interface_index_by_sw_if_index, 
                    tx_sw_if_index);
      if (p == 0)
        {
          clib_warning ("sw_if_index %d unknown", tx_sw_if_index);
          /* $$$ leak, but this should never happen... */
          continue;
        }
      else
        ti = vec_elt_at_index (tm->tapcli_interfaces, p[0]);

      /* Re-set iovecs if present. */
      if (tm->iovecs)
	_vec_len (tm->iovecs) = 0;

      /* VLIB buffer chain -> Unix iovec(s). */
      vec_add2 (tm->iovecs, iov, 1);
      iov->iov_base = b->data + b->current_data;
      iov->iov_len = l = b->current_length;

      if (PREDICT_FALSE (b->flags & VLIB_BUFFER_NEXT_PRESENT))
	{
	  do {
	    b = vlib_get_buffer (vm, b->next_buffer);

	    vec_add2 (tm->iovecs, iov, 1);

	    iov->iov_base = b->data + b->current_data;
	    iov->iov_len = b->current_length;
	    l += b->current_length;
	  } while (b->flags & VLIB_BUFFER_NEXT_PRESENT);
	}

      if (writev (ti->unix_fd, tm->iovecs, vec_len (tm->iovecs)) < l)
	clib_unix_warning ("writev");
    }
    
  vlib_buffer_free(vm, vlib_frame_vector_args(frame), frame->n_vectors);
    
  return n_packets;
}

VLIB_REGISTER_NODE (tapcli_tx_node,static) = {
  .function = tapcli_tx,
  .name = "tapcli-tx",
  .type = VLIB_NODE_TYPE_INTERNAL,
  .vector_size = 4,
};

enum {
  TAPCLI_RX_NEXT_IP4_INPUT, 
  TAPCLI_RX_NEXT_IP6_INPUT, 
  TAPCLI_RX_NEXT_ETHERNET_INPUT,
  TAPCLI_RX_NEXT_DROP,
  TAPCLI_RX_N_NEXT,
};



static uword tapcli_rx_iface(vlib_main_t * vm,
                            vlib_node_runtime_t * node,
                            tapcli_interface_t * ti)
{
  tapcli_main_t * tm = &tapcli_main;
  const uword buffer_size = VLIB_BUFFER_DATA_SIZE;
  u32 n_trace = vlib_get_trace_count (vm, node);
  u8 set_trace = 0;

  vnet_main_t *vnm;
  vnet_sw_interface_t * si;
  u8 admin_down;
  u32 next = node->cached_next_index;
  u32 n_left_to_next, next_index;
  u32 *to_next;

  vnm = vnet_get_main();
  si = vnet_get_sw_interface (vnm, ti->sw_if_index);
  admin_down = !(si->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP);

  vlib_get_next_frame(vm, node, next, to_next, n_left_to_next);

  while (n_left_to_next) { // Fill at most one vector
    vlib_buffer_t *b_first, *b, *prev;
    u32 bi_first, bi;
    word n_bytes_in_packet;
    int j, n_bytes_left;

    if (PREDICT_FALSE(vec_len(tm->rx_buffers) < tm->mtu_buffers)) {
      uword len = vec_len(tm->rx_buffers);
      _vec_len(tm->rx_buffers) +=
          vlib_buffer_alloc_from_free_list(vm, &tm->rx_buffers[len],
                            VLIB_FRAME_SIZE - len, VLIB_BUFFER_DEFAULT_FREE_LIST_INDEX);
      if (PREDICT_FALSE(vec_len(tm->rx_buffers) < tm->mtu_buffers)) {
        clib_warning("vlib_buffer_alloc failed");
        break;
      }
    }

    uword i_rx = vec_len (tm->rx_buffers) - 1;

    /* Allocate RX buffers from end of rx_buffers.
           Turn them into iovecs to pass to readv. */
    vec_validate (tm->iovecs, tm->mtu_buffers - 1);
    for (j = 0; j < tm->mtu_buffers; j++) {
      b = vlib_get_buffer (vm, tm->rx_buffers[i_rx - j]);
      b->clone_count = 0;
      tm->iovecs[j].iov_base = b->data;
      tm->iovecs[j].iov_len = buffer_size;
    }

    n_bytes_left = readv (ti->unix_fd, tm->iovecs, tm->mtu_buffers);
    n_bytes_in_packet = n_bytes_left;
    if (n_bytes_left <= 0) {
      if (errno != EAGAIN) {
        vlib_node_increment_counter(vm, tapcli_rx_node.index,
                                    TAPCLI_ERROR_READ, 1);
      }
      break;
    }

    bi_first = tm->rx_buffers[i_rx];
    b = b_first = vlib_get_buffer (vm, tm->rx_buffers[i_rx]);
    prev = NULL;

    while (1) {
      b->current_length = n_bytes_left < buffer_size ? n_bytes_left : buffer_size;
      n_bytes_left -= buffer_size;

      if (prev) {
        prev->next_buffer = bi;
        prev->flags |= VLIB_BUFFER_NEXT_PRESENT;
      }
      prev = b;

      /* last segment */
      if (n_bytes_left <= 0)
        break;

      i_rx--;
      bi = tm->rx_buffers[i_rx];
      b = vlib_get_buffer (vm, bi);
    }

    _vec_len (tm->rx_buffers) = i_rx;

    b_first->total_length_not_including_first_buffer =
        (n_bytes_in_packet > buffer_size) ? n_bytes_in_packet - buffer_size : 0;
    b_first->flags |= VLIB_BUFFER_TOTAL_LENGTH_VALID;

    /* Ensure mbufs are updated */
    vlib_buffer_chain_validate(vm, b_first);

    VLIB_BUFFER_TRACE_TRAJECTORY_INIT(b_first);

    vnet_buffer (b_first)->sw_if_index[VLIB_RX] = ti->sw_if_index;
    vnet_buffer (b_first)->sw_if_index[VLIB_TX] = (u32)~0;

    b_first->error = node->errors[TAPCLI_ERROR_NONE];
    next_index = TAPCLI_RX_NEXT_ETHERNET_INPUT;
    next_index = (ti->per_interface_next_index != ~0) ?
        ti->per_interface_next_index : next_index;
    next_index = admin_down ? TAPCLI_RX_NEXT_DROP : next_index;

    to_next[0] = bi_first;
    to_next++;
    n_left_to_next--;

    vlib_validate_buffer_enqueue_x1 (vm, node, next,
                                     to_next, n_left_to_next,
                                     bi_first, next_index);

    /* Interface counters for tapcli interface. */
    if (PREDICT_TRUE(!admin_down)) {
      vlib_increment_combined_counter (
          vnet_main.interface_main.combined_sw_if_counters
          + VNET_INTERFACE_COUNTER_RX,
          os_get_cpu_number(), ti->sw_if_index,
          1, n_bytes_in_packet);

      if (PREDICT_FALSE(n_trace > 0)) {
        vlib_trace_buffer (vm, node, next_index,
                           b_first, /* follow_chain */ 1);
        n_trace--;
        set_trace = 1;
        tapcli_rx_trace_t *t0 = vlib_add_trace (vm, node, b_first, sizeof (*t0));
        t0->sw_if_index = si->sw_if_index;
      }
    }
  }
  vlib_put_next_frame (vm, node, next, n_left_to_next);
  if (set_trace)
    vlib_set_trace_count (vm, node, n_trace);
  return VLIB_FRAME_SIZE - n_left_to_next;
}

static uword
tapcli_rx (vlib_main_t * vm,
	   vlib_node_runtime_t * node,
	   vlib_frame_t * frame)
{
  tapcli_main_t * tm = &tapcli_main;
  static u32 * ready_interface_indices;
  tapcli_interface_t * ti;
  int i;
  u32 total_count = 0;

  vec_reset_length (ready_interface_indices);
  clib_bitmap_foreach (i, tm->pending_read_bitmap,
  ({
    vec_add1 (ready_interface_indices, i);
  }));

  if (vec_len (ready_interface_indices) == 0)
    return 1;

  for (i = 0; i < vec_len(ready_interface_indices); i++)
  {
    tm->pending_read_bitmap =
        clib_bitmap_set (tm->pending_read_bitmap,
                         ready_interface_indices[i], 0);

    ti = vec_elt_at_index (tm->tapcli_interfaces, ready_interface_indices[i]);
    total_count += tapcli_rx_iface(vm, node, ti);
  }
  return total_count; //This might return more than 256.
}

static char * tapcli_rx_error_strings[] = {
#define _(sym,string) string,
  foreach_tapcli_error
#undef _
};

VLIB_REGISTER_NODE (tapcli_rx_node, static) = {
  .function = tapcli_rx,
  .name = "tapcli-rx",
  .type = VLIB_NODE_TYPE_INPUT,
  .state = VLIB_NODE_STATE_INTERRUPT,
  .vector_size = 4,
  .n_errors = TAPCLI_N_ERROR,
  .error_strings = tapcli_rx_error_strings,
  .format_trace = format_tapcli_rx_trace,

  .n_next_nodes = TAPCLI_RX_N_NEXT,
  .next_nodes = {
    [TAPCLI_RX_NEXT_IP4_INPUT] = "ip4-input-no-checksum",
    [TAPCLI_RX_NEXT_IP6_INPUT] = "ip6-input",
    [TAPCLI_RX_NEXT_DROP] = "error-drop",
    [TAPCLI_RX_NEXT_ETHERNET_INPUT] = "ethernet-input",
  },
};

/* Gets called when file descriptor is ready from epoll. */
static clib_error_t * tapcli_read_ready (unix_file_t * uf)
{
  vlib_main_t * vm = vlib_get_main();
  tapcli_main_t * tm = &tapcli_main;
  uword * p;
  
  /* Schedule the rx node */
  vlib_node_set_interrupt_pending (vm, tapcli_rx_node.index);

  p = hash_get (tm->tapcli_interface_index_by_unix_fd, uf->file_descriptor);

  /* Mark the specific tap interface ready-to-read */
  if (p)
    tm->pending_read_bitmap = clib_bitmap_set (tm->pending_read_bitmap,
                                               p[0], 1);
  else
    clib_warning ("fd %d not in hash table", uf->file_descriptor);

  return 0;
}

static clib_error_t *
tapcli_config (vlib_main_t * vm, unformat_input_t * input)
{
  tapcli_main_t *tm = &tapcli_main;
  const uword buffer_size = VLIB_BUFFER_DATA_SIZE;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "mtu %d", &tm->mtu_bytes))
	;
      else if (unformat (input, "disable"))
        tm->is_disabled = 1;
      else
          return clib_error_return (0, "unknown input `%U'",
                                    format_unformat_error, input);
    }

  if (tm->is_disabled)
    return 0;

  if (geteuid()) 
    {
      clib_warning ("tapcli disabled: must be superuser");
      tm->is_disabled = 1;
      return 0;
    }    

  tm->mtu_buffers = (tm->mtu_bytes + (buffer_size - 1)) / buffer_size;
  
  return 0;
}

static int tap_name_renumber (vnet_hw_interface_t * hi,
                              u32 new_dev_instance)
{
  tapcli_main_t *tm = &tapcli_main;

  vec_validate_init_empty (tm->show_dev_instance_by_real_dev_instance,
                           hi->dev_instance, ~0);

  tm->show_dev_instance_by_real_dev_instance [hi->dev_instance] =
    new_dev_instance;

  return 0;
}

VLIB_CONFIG_FUNCTION (tapcli_config, "tapcli");

static void
tapcli_nopunt_frame (vlib_main_t * vm,
                   vlib_node_runtime_t * node,
                   vlib_frame_t * frame)
{
  u32 * buffers = vlib_frame_args (frame);
  uword n_packets = frame->n_vectors;
  vlib_buffer_free (vm, buffers, n_packets);
  vlib_frame_free (vm, node, frame);
}

VNET_HW_INTERFACE_CLASS (tapcli_interface_class,static) = {
  .name = "tapcli",
};

static u8 * format_tapcli_interface_name (u8 * s, va_list * args)
{
  u32 i = va_arg (*args, u32);
  u32 show_dev_instance = ~0;
  tapcli_main_t * tm = &tapcli_main;

  if (i < vec_len (tm->show_dev_instance_by_real_dev_instance))
    show_dev_instance = tm->show_dev_instance_by_real_dev_instance[i];

  if (show_dev_instance != ~0)
    i = show_dev_instance;

  s = format (s, "tap-%d", i);
  return s;
}

static u32 tapcli_flag_change (vnet_main_t * vnm, 
                               vnet_hw_interface_t * hw,
                               u32 flags)
{
  tapcli_main_t *tm = &tapcli_main;
  tapcli_interface_t *ti;

   ti = vec_elt_at_index (tm->tapcli_interfaces, hw->dev_instance);

  if (flags & ETHERNET_INTERFACE_FLAG_MTU)
    {
      const uword buffer_size = VLIB_BUFFER_DATA_SIZE;
      tm->mtu_bytes = hw->max_packet_bytes;
      tm->mtu_buffers = (tm->mtu_bytes + (buffer_size - 1)) / buffer_size;
    }
   else
    {
      struct ifreq ifr;
      u32 want_promisc;

      memcpy (&ifr, &ti->ifr, sizeof (ifr));

      /* get flags, modify to bring up interface... */
      if (ioctl (ti->provision_fd, SIOCGIFFLAGS, &ifr) < 0)
        {
          clib_unix_warning ("Couldn't get interface flags for %s", hw->name);
          return 0;
        }

      want_promisc = (flags & ETHERNET_INTERFACE_FLAG_ACCEPT_ALL) != 0;

      if (want_promisc == ti->is_promisc)
        return 0;

      if (flags & ETHERNET_INTERFACE_FLAG_ACCEPT_ALL)
        ifr.ifr_flags |= IFF_PROMISC;
      else
        ifr.ifr_flags &= ~(IFF_PROMISC);

      /* get flags, modify to bring up interface... */
      if (ioctl (ti->provision_fd, SIOCSIFFLAGS, &ifr) < 0)
        {
          clib_unix_warning ("Couldn't set interface flags for %s", hw->name);
          return 0;
        }

      ti->is_promisc = want_promisc;
    }

  return 0;
}

static void tapcli_set_interface_next_node (vnet_main_t *vnm, 
                                            u32 hw_if_index,
                                            u32 node_index)
{
  tapcli_main_t *tm = &tapcli_main;
  tapcli_interface_t *ti;
  vnet_hw_interface_t *hw = vnet_get_hw_interface (vnm, hw_if_index);

  ti = vec_elt_at_index (tm->tapcli_interfaces, hw->dev_instance);
  
  /* Shut off redirection */
  if (node_index == ~0)
    {
      ti->per_interface_next_index = node_index;
      return;
    }
  
  ti->per_interface_next_index = 
    vlib_node_add_next (tm->vlib_main, tapcli_rx_node.index, node_index);
}

/* 
 * Mainly exists to set link_state == admin_state
 * otherwise, e.g. ip6 neighbor discovery breaks
 */
static clib_error_t * 
tapcli_interface_admin_up_down (vnet_main_t * vnm, u32 hw_if_index, u32 flags)
{
  uword is_admin_up = (flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP) != 0;
  u32 hw_flags;
  u32 speed_duplex = VNET_HW_INTERFACE_FLAG_FULL_DUPLEX 
    | VNET_HW_INTERFACE_FLAG_SPEED_1G;
    
  if (is_admin_up)
    hw_flags = VNET_HW_INTERFACE_FLAG_LINK_UP | speed_duplex;
  else
    hw_flags = speed_duplex;
  
  vnet_hw_interface_set_flags (vnm, hw_if_index, hw_flags);
  return 0;
}

VNET_DEVICE_CLASS (tapcli_dev_class,static) = {
  .name = "tapcli",
  .tx_function = tapcli_tx,
  .format_device_name = format_tapcli_interface_name,
  .rx_redirect_to_node = tapcli_set_interface_next_node,
  .name_renumber = tap_name_renumber,
  .admin_up_down_function = tapcli_interface_admin_up_down,
  .no_flatten_output_chains = 1,
};

int vnet_tap_dump_ifs (tapcli_interface_details_t **out_tapids)
{
  tapcli_main_t * tm = &tapcli_main;
  tapcli_interface_t * ti;

  tapcli_interface_details_t * r_tapids = NULL;
  tapcli_interface_details_t * tapid = NULL;

  vec_foreach (ti, tm->tapcli_interfaces) {
    if (!ti->active)
        continue;
    vec_add2(r_tapids, tapid, 1);
    tapid->sw_if_index = ti->sw_if_index;
    strncpy((char *)tapid->dev_name, ti->ifr.ifr_name, sizeof (ti->ifr.ifr_name)-1);
  }

  *out_tapids = r_tapids;

  return 0;
}

/* get tap interface from inactive interfaces or create new */
static tapcli_interface_t *tapcli_get_new_tapif()
{
  tapcli_main_t * tm = &tapcli_main;
  tapcli_interface_t *ti = NULL;

  int inactive_cnt = vec_len(tm->tapcli_inactive_interfaces);
  // if there are any inactive ifaces
  if (inactive_cnt > 0) {
    // take last
    u32 ti_idx = tm->tapcli_inactive_interfaces[inactive_cnt - 1];
    if (vec_len(tm->tapcli_interfaces) > ti_idx) {
      ti = vec_elt_at_index (tm->tapcli_interfaces, ti_idx);
      clib_warning("reusing tap interface");
    }
    // "remove" from inactive list
    _vec_len(tm->tapcli_inactive_interfaces) -= 1;
  }

  // ti was not retrieved from inactive ifaces - create new
  if (!ti)
    vec_add2 (tm->tapcli_interfaces, ti, 1);

  return ti;
}

int vnet_tap_connect (vlib_main_t * vm, u8 * intfc_name, u8 *hwaddr_arg,
                      u32 * sw_if_indexp)
{
  tapcli_main_t * tm = &tapcli_main;
  tapcli_interface_t * ti = NULL;
  struct ifreq ifr;
  int flags;
  int dev_net_tun_fd;
  int dev_tap_fd = -1;
  clib_error_t * error;
  u8 hwaddr [6];
  int rv = 0;

  if (tm->is_disabled)
    {
      return VNET_API_ERROR_FEATURE_DISABLED;
    }

  flags = IFF_TAP | IFF_NO_PI;

  if ((dev_net_tun_fd = open ("/dev/net/tun", O_RDWR)) < 0)
    return VNET_API_ERROR_SYSCALL_ERROR_1;
  
  memset (&ifr, 0, sizeof (ifr));
  strncpy(ifr.ifr_name, (char *) intfc_name, sizeof (ifr.ifr_name)-1);
  ifr.ifr_flags = flags;
  if (ioctl (dev_net_tun_fd, TUNSETIFF, (void *)&ifr) < 0)
    {
      rv = VNET_API_ERROR_SYSCALL_ERROR_2;
      goto error;
    }
    
  /* Open a provisioning socket */
  if ((dev_tap_fd = socket(PF_PACKET, SOCK_RAW,
                           htons(ETH_P_ALL))) < 0 )
    {
      rv = VNET_API_ERROR_SYSCALL_ERROR_3;
      goto error;
    }

  /* Find the interface index. */
  {
    struct ifreq ifr;
    struct sockaddr_ll sll;

    memset (&ifr, 0, sizeof(ifr));
    strncpy (ifr.ifr_name, (char *) intfc_name, sizeof (ifr.ifr_name)-1);
    if (ioctl (dev_tap_fd, SIOCGIFINDEX, &ifr) < 0 )
      {
        rv = VNET_API_ERROR_SYSCALL_ERROR_4;
        goto error;
      }

    /* Bind the provisioning socket to the interface. */
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_ifindex  = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(dev_tap_fd, (struct sockaddr*) &sll, sizeof(sll)) < 0)
      {
        rv = VNET_API_ERROR_SYSCALL_ERROR_5;
        goto error;
      }
  }

  /* non-blocking I/O on /dev/tapX */
  {
    int one = 1;
    if (ioctl (dev_net_tun_fd, FIONBIO, &one) < 0)
      {
        rv = VNET_API_ERROR_SYSCALL_ERROR_6;
	goto error;
      }
  }
  ifr.ifr_mtu = tm->mtu_bytes;
  if (ioctl (dev_tap_fd, SIOCSIFMTU, &ifr) < 0)
    {
      rv = VNET_API_ERROR_SYSCALL_ERROR_7;
      goto error;
    }

  /* get flags, modify to bring up interface... */
  if (ioctl (dev_tap_fd, SIOCGIFFLAGS, &ifr) < 0)
    {
      rv = VNET_API_ERROR_SYSCALL_ERROR_8;
      goto error;
    }

  ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);

  if (ioctl (dev_tap_fd, SIOCSIFFLAGS, &ifr) < 0)
    {
      rv = VNET_API_ERROR_SYSCALL_ERROR_9;
      goto error;
    }

  if (ioctl (dev_tap_fd, SIOCGIFHWADDR, &ifr) < 0)
    {
      rv = VNET_API_ERROR_SYSCALL_ERROR_1;
      goto error;
    }

  ti = tapcli_get_new_tapif();
  ti->per_interface_next_index = ~0;

  if (hwaddr_arg != 0)
    clib_memcpy(hwaddr, hwaddr_arg, 6);

  error = ethernet_register_interface
        (tm->vnet_main,
         tapcli_dev_class.index,
         ti - tm->tapcli_interfaces /* device instance */,
         hwaddr_arg != 0 ? hwaddr :
         (u8 *) ifr.ifr_hwaddr.sa_data /* ethernet address */,
         &ti->hw_if_index, 
         tapcli_flag_change);

  if (error)
    {
      clib_error_report (error);
      rv = VNET_API_ERROR_INVALID_REGISTRATION;
      goto error;
    }

  {
    unix_file_t template = {0};
    template.read_function = tapcli_read_ready;
    template.file_descriptor = dev_net_tun_fd;
    ti->unix_file_index = unix_file_add (&unix_main, &template);
    ti->unix_fd = dev_net_tun_fd;
    ti->provision_fd = dev_tap_fd;
    clib_memcpy (&ti->ifr, &ifr, sizeof (ifr));
  }
  
  {
    vnet_hw_interface_t * hw;
    hw = vnet_get_hw_interface (tm->vnet_main, ti->hw_if_index);
    hw->min_supported_packet_bytes = TAP_MTU_MIN;
    hw->max_supported_packet_bytes = TAP_MTU_MAX;
    hw->max_l3_packet_bytes[VLIB_RX] = hw->max_l3_packet_bytes[VLIB_TX] = hw->max_supported_packet_bytes - sizeof(ethernet_header_t);
    ti->sw_if_index = hw->sw_if_index;
    if (sw_if_indexp)
      *sw_if_indexp = hw->sw_if_index;
  }

  ti->active = 1;
  
  hash_set (tm->tapcli_interface_index_by_sw_if_index, ti->sw_if_index,
            ti - tm->tapcli_interfaces);
  
  hash_set (tm->tapcli_interface_index_by_unix_fd, ti->unix_fd,
            ti - tm->tapcli_interfaces);
  
  return rv;

 error:
  close (dev_net_tun_fd);
  close (dev_tap_fd);

  return rv;
}

int vnet_tap_connect_renumber (vlib_main_t * vm, u8 * intfc_name,
                               u8 *hwaddr_arg, u32 * sw_if_indexp,
                               u8 renumber, u32 custom_dev_instance)
{
    int rv = vnet_tap_connect(vm, intfc_name, hwaddr_arg, sw_if_indexp);

    if (!rv && renumber)
        vnet_interface_name_renumber (*sw_if_indexp, custom_dev_instance);

    return rv;
}

static int tapcli_tap_disconnect (tapcli_interface_t *ti)
{
  int rv = 0;
  vnet_main_t * vnm = vnet_get_main();
  tapcli_main_t * tm = &tapcli_main;
  u32 sw_if_index = ti->sw_if_index;

  // bring interface down
  vnet_sw_interface_set_flags (vnm, sw_if_index, 0);

  if (ti->unix_file_index != ~0) {
    unix_file_del (&unix_main, unix_main.file_pool + ti->unix_file_index);
    ti->unix_file_index = ~0;
  }

  hash_unset (tm->tapcli_interface_index_by_unix_fd, ti->unix_fd);
  hash_unset (tm->tapcli_interface_index_by_sw_if_index, ti->sw_if_index);
  close(ti->unix_fd);
  close(ti->provision_fd);
  ti->unix_fd = -1;
  ti->provision_fd = -1;

  return rv;
}

int vnet_tap_delete(vlib_main_t *vm, u32 sw_if_index)
{
  int rv = 0;
  tapcli_main_t * tm = &tapcli_main;
  tapcli_interface_t *ti;
  uword *p = NULL;

  p = hash_get (tm->tapcli_interface_index_by_sw_if_index,
                sw_if_index);
  if (p == 0) {
    clib_warning ("sw_if_index %d unknown", sw_if_index);
    return VNET_API_ERROR_INVALID_SW_IF_INDEX;
  }
  ti = vec_elt_at_index (tm->tapcli_interfaces, p[0]);

  // inactive
  ti->active = 0;
  tapcli_tap_disconnect(ti);
  // add to inactive list
  vec_add1(tm->tapcli_inactive_interfaces, ti - tm->tapcli_interfaces);

  // reset renumbered iface
  if (p[0] < vec_len (tm->show_dev_instance_by_real_dev_instance))
    tm->show_dev_instance_by_real_dev_instance[p[0]] = ~0;

  ethernet_delete_interface (tm->vnet_main, ti->hw_if_index);
  return rv;
}

static clib_error_t *
tap_delete_command_fn (vlib_main_t * vm,
		 unformat_input_t * input,
		 vlib_cli_command_t * cmd)
{
  tapcli_main_t * tm = &tapcli_main;
  u32 sw_if_index = ~0;

  if (tm->is_disabled)
    {
      return clib_error_return (0, "device disabled...");
    }

  if (unformat (input, "%U", unformat_vnet_sw_interface, tm->vnet_main,
                &sw_if_index))
      ;
  else
    return clib_error_return (0, "unknown input `%U'",
                              format_unformat_error, input);


  int rc = vnet_tap_delete (vm, sw_if_index);

  if (!rc) {
    vlib_cli_output (vm, "Deleted.");
  } else {
    vlib_cli_output (vm, "Error during deletion of tap interface. (rc: %d)", rc);
  }

  return 0;
}

VLIB_CLI_COMMAND (tap_delete_command, static) = {
    .path = "tap delete",
    .short_help = "tap delete <vpp-tap-intfc-name>",
    .function = tap_delete_command_fn,
};

/* modifies tap interface - can result in new interface being created */
int vnet_tap_modify (vlib_main_t * vm, u32 orig_sw_if_index,
                     u8 * intfc_name, u8 *hwaddr_arg, 
                     u32 * sw_if_indexp,
                     u8 renumber, u32 custom_dev_instance)
{
    int rv = vnet_tap_delete (vm, orig_sw_if_index);

    if (rv)
        return rv;

    rv = vnet_tap_connect_renumber(vm, intfc_name, hwaddr_arg, sw_if_indexp,
            renumber, custom_dev_instance);

    return rv;
}

static clib_error_t *
tap_modify_command_fn (vlib_main_t * vm,
		 unformat_input_t * input,
		 vlib_cli_command_t * cmd)
{
  u8 * intfc_name;
  tapcli_main_t * tm = &tapcli_main;
  u32 sw_if_index = ~0;
  u32 new_sw_if_index = ~0;
  int user_hwaddr = 0;
  u8 hwaddr[6];
    
  if (tm->is_disabled)
    {
      return clib_error_return (0, "device disabled...");
    }

  if (unformat (input, "%U", unformat_vnet_sw_interface, tm->vnet_main,
                &sw_if_index))
      ;
  else
    return clib_error_return (0, "unknown input `%U'",
                              format_unformat_error, input);

  if (unformat (input, "%s", &intfc_name))
    ;
  else
    return clib_error_return (0, "unknown input `%U'",
                              format_unformat_error, input);
  
  if (unformat(input, "hwaddr %U", unformat_ethernet_address,
               &hwaddr))
    user_hwaddr = 1;


  int rc = vnet_tap_modify (vm, sw_if_index, intfc_name,
                            (user_hwaddr == 1 ? hwaddr : 0),
                            &new_sw_if_index, 0, 0);

  if (!rc) {
    vlib_cli_output (vm, "Modified %U for Linux tap '%s'",
                   format_vnet_sw_if_index_name, tm->vnet_main, 
                   new_sw_if_index, intfc_name);
  } else {
    vlib_cli_output (vm, "Error during modification of tap interface. (rc: %d)", rc);
  }

  return 0;
}

VLIB_CLI_COMMAND (tap_modify_command, static) = {
    .path = "tap modify",
    .short_help = "tap modify <vpp-tap-intfc-name> <linux-intfc-name> [hwaddr [<addr> | random]]",
    .function = tap_modify_command_fn,
};

static clib_error_t *
tap_connect_command_fn (vlib_main_t * vm,
		 unformat_input_t * input,
		 vlib_cli_command_t * cmd)
{
  u8 * intfc_name;
  tapcli_main_t * tm = &tapcli_main;
  int user_hwaddr = 0;
  u8 hwaddr[6];
  u8 *hwaddr_arg = 0;
  u32 sw_if_index;

  if (tm->is_disabled)
    {
      return clib_error_return (0, "device disabled...");
    }

  if (unformat (input, "%s", &intfc_name))
    ;
  else
    return clib_error_return (0, "unknown input `%U'",
                              format_unformat_error, input);
  
  if (unformat(input, "hwaddr %U", unformat_ethernet_address,
               &hwaddr))
    user_hwaddr = 1;

  if (unformat(input, "hwaddr random"))
    {
      f64 now = vlib_time_now(vm);
      u32 rnd;
      rnd = (u32) (now * 1e6);
      rnd = random_u32 (&rnd);

      clib_memcpy (hwaddr+2, &rnd, sizeof(rnd));
      hwaddr[0] = 2;
      hwaddr[1] = 0xfe;
      user_hwaddr = 1;
    }
  if (user_hwaddr) hwaddr_arg = hwaddr;

  int rv = vnet_tap_connect(vm, intfc_name, hwaddr_arg, &sw_if_index);
  if (rv) {
    switch (rv) {
    case VNET_API_ERROR_SYSCALL_ERROR_1:
      vlib_cli_output (vm, "Couldn't open /dev/net/tun");
      break;

    case VNET_API_ERROR_SYSCALL_ERROR_2:
      vlib_cli_output (vm, "Error setting flags on '%s'", intfc_name);
      break;
  
    case VNET_API_ERROR_SYSCALL_ERROR_3:
      vlib_cli_output (vm, "Couldn't open provisioning socket");
      break;

    case VNET_API_ERROR_SYSCALL_ERROR_4:
      vlib_cli_output (vm, "Couldn't get if_index");
      break;
    
    case VNET_API_ERROR_SYSCALL_ERROR_5:
      vlib_cli_output (vm, "Couldn't bind provisioning socket");
      break;

    case VNET_API_ERROR_SYSCALL_ERROR_6:
      vlib_cli_output (0, "Couldn't set device non-blocking flag");
      break;

    case VNET_API_ERROR_SYSCALL_ERROR_7:
      vlib_cli_output (0, "Couldn't set device MTU");
      break;

    case VNET_API_ERROR_SYSCALL_ERROR_8:
      vlib_cli_output (0, "Couldn't get interface flags");
      break;

    case VNET_API_ERROR_SYSCALL_ERROR_9:
      vlib_cli_output (0, "Couldn't set intfc admin state up");
      break;

    case VNET_API_ERROR_INVALID_REGISTRATION:
      vlib_cli_output (0, "Invalid registration");
      break;
    default:
      vlib_cli_output (0, "Unknown error: %d", rv);
      break;
    }
    return 0;
  }

  vlib_cli_output (vm, "Created %U for Linux tap '%s'",
		   format_vnet_sw_if_index_name, tm->vnet_main, 
		   sw_if_index, intfc_name);
                   
  return 0;

  }

VLIB_CLI_COMMAND (tap_connect_command, static) = {
    .path = "tap connect",
    .short_help = "tap connect <intfc-name> [hwaddr [<addr> | random]]",
    .function = tap_connect_command_fn,
};

clib_error_t *
tapcli_init (vlib_main_t * vm)
{
  tapcli_main_t * tm = &tapcli_main;

  tm->vlib_main = vm;
  tm->vnet_main = vnet_get_main();
  tm->unix_main = &unix_main;
  tm->mtu_bytes = TAP_MTU_DEFAULT;
  tm->tapcli_interface_index_by_sw_if_index = hash_create (0, sizeof(uword));
  tm->tapcli_interface_index_by_unix_fd = hash_create (0, sizeof (uword));
  tm->rx_buffers = 0;
  vec_alloc(tm->rx_buffers, VLIB_FRAME_SIZE);
  vec_reset_length(tm->rx_buffers);
  vm->os_punt_frame = tapcli_nopunt_frame;
  return 0;
}

VLIB_INIT_FUNCTION (tapcli_init);
