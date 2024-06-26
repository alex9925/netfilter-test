#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <errno.h>
#include <string.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

#include "header.h"


char number;
char **hostname;

void dump(unsigned char* buf, int size) {
   int i;
   for (i = 0; i < size; i++) {
      if (i != 0 && i % 16 == 0)
         printf("\n");
      printf("%02X ", buf[i]);
   }
   printf("\n");
}

/* returns packet id */
static uint32_t print_pkt (struct nfq_data *tb)
{
   int id = 0;
   struct nfqnl_msg_packet_hdr *ph;
   struct nfqnl_msg_packet_hw *hwph;
   uint32_t mark, ifi, uid, gid; //uid, git 추가
   int ret;
   unsigned char *data, *secdata; //secdata 추가

   ph = nfq_get_msg_packet_hdr(tb);
   if (ph) {
      id = ntohl(ph->packet_id);
      printf("hw_protocol=0x%04x hook=%u id=%u ",
         ntohs(ph->hw_protocol), ph->hook, id);
   }

   hwph = nfq_get_packet_hw(tb);
   if (hwph) {
      int i, hlen = ntohs(hwph->hw_addrlen);

      printf("hw_src_addr=");
      for (i = 0; i < hlen-1; i++)
         printf("%02x:", hwph->hw_addr[i]);
      printf("%02x ", hwph->hw_addr[hlen-1]);
   }

   mark = nfq_get_nfmark(tb);
   if (mark)
      printf("mark=%u ", mark);

   ifi = nfq_get_indev(tb);
   if (ifi)
      printf("indev=%u ", ifi);

   ifi = nfq_get_outdev(tb);
   if (ifi)
      printf("outdev=%u ", ifi);
   ifi = nfq_get_physindev(tb);
   if (ifi)
      printf("physindev=%u ", ifi);

   ifi = nfq_get_physoutdev(tb);
   if (ifi)
      printf("physoutdev=%u ", ifi);

  
   if (nfq_get_uid(tb, &uid))
      printf("uid=%u ", uid);

   if (nfq_get_gid(tb, &gid))
      printf("gid=%u ", gid);

   ret = nfq_get_secctx(tb, &secdata);
   if (ret > 0)
      printf("secctx=\"%.*s\" ", ret, secdata);

   ret = nfq_get_payload(tb, &data);
   if (ret >= 0){
      printf("\n");
      dump(data, ret);

      printf("payload_len=%d ", ret);
   }

   fputc('\n', stdout);

   return id;
}

static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
         struct nfq_data *nfa, void *data)
{
   uint32_t id = print_pkt(nfa);
   printf("entering callback\n\n");
   
   unsigned char *pkt_d;
   int temp = nfq_get_payload(nfa, &pkt_d);
   char *site_name = NULL;

   //IP 파싱
   IpHdr *iphdr = (IpHdr *)pkt_d;
   //Protocol 내에서 header가 TCP:0x06가 아니라면 받아옴 
   if (iphdr->protocol != 0x06) {
      return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
   }


   //TCP 파싱
   TcpHdr *tcphdr = (TcpHdr *)(pkt_d + sizeof(IpHdr));
   //HTTP 파싱
   unsigned char *httphdr = (unsigned char *)(pkt_d + sizeof(IpHdr) + (tcphdr->offset * 4));
   
   if ((ntohs(tcphdr->d_port) == 80) && (strncmp(httphdr, "GET", 3) == 0) && ((site_name = strstr(httphdr, "Host: ")) != NULL)) {
      for (int i = 1; i < number; i++) {
         if (strncmp(site_name + 6, hostname[i], strlen(hostname[i])) == 0) {
            printf("site name : %s is blocked!\n\n\n", hostname[i]);
            return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
         }
      }
   }

   return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
}

int main(int argc, char **argv)
{
   struct nfq_handle *h;
   struct nfq_q_handle *qh;
   int fd;
   int rv;
   uint32_t queue = 0;
   char buf[4096] __attribute__ ((aligned));

   if (argc < 2) {
      printf("syntax : netfilter-test <host>\n");
      printf("sample : netfilter-test test.gilgil.net\n");

      return 0;
   }

   //전역변수 값 설정
   number = argc;
   hostname = argv;

   printf("opening library handle\n");
   h = nfq_open();
   if (!h) {
      fprintf(stderr, "error during nfq_open()\n");
      exit(1);
   }

   printf("unbinding existing nf_queue handler for AF_INET (if any)\n");
   if (nfq_unbind_pf(h, AF_INET) < 0) {
      fprintf(stderr, "error during nfq_unbind_pf()\n");
      exit(1);
   }

   printf("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
   if (nfq_bind_pf(h, AF_INET) < 0) {
      fprintf(stderr, "error during nfq_bind_pf()\n");
      exit(1);
   }

   printf("binding this socket to queue '%d'\n", queue);
   qh = nfq_create_queue(h, queue, &cb, NULL);
   if (!qh) {
      fprintf(stderr, "error during nfq_create_queue()\n");
      exit(1);
   }

   printf("setting copy_packet mode\n");
   if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
      fprintf(stderr, "can't set packet_copy mode\n");
      exit(1);
   }

   printf("setting flags to request UID and GID\n");
   if (nfq_set_queue_flags(qh, NFQA_CFG_F_UID_GID, NFQA_CFG_F_UID_GID)) {
      fprintf(stderr, "This kernel version does not allow to "
            "retrieve process UID/GID.\n");
   }

   printf("setting flags to request security context\n");
   if (nfq_set_queue_flags(qh, NFQA_CFG_F_SECCTX, NFQA_CFG_F_SECCTX)) {
      fprintf(stderr, "This kernel version does not allow to "
            "retrieve security context.\n");
   }

   printf("Waiting for packets..\n"); 
   
   fd = nfq_fd(h);

   for (;;) {
      if ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
         printf("pkt received!\n");
         nfq_handle_packet(h, buf, rv);
         continue;
      }
      /* if your application is too slow to digest the packets that
       * are sent from kernel-space, the socket buffer that we use
       * to enqueue packets may fill up returning ENOBUFS. Depending
       * on your application, this error may be ignored. Please, see
       * the doxygen documentation of this library on how to improve
       * this situation.
       */
      if (rv < 0 && errno == ENOBUFS) {
         printf("losing packets!\n");
         continue;
      }
      perror("recv failed");
      break;
   }

   printf("unbinding from queue 0\n");
   nfq_destroy_queue(qh);

#ifdef INSANE
   /* normally, applications SHOULD NOT issue this command, since
    * it detaches other programs/sockets from AF_INET, too ! */
   printf("unbinding from AF_INET\n");
   nfq_unbind_pf(h, AF_INET);
#endif

   printf("closing library handle\n");
   nfq_close(h);

   exit(0);
}
