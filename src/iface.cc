// ndppd - NDP Proxy Daemon
// Copyright (C) 2011  Daniel Adolfsson <daniel.adolfsson@tuhox.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <netinet/ether.h>
#include <netpacket/packet.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>

#include <linux/filter.h>

#include <string>
#include <vector>
#include <map>

#include "ndppd.h"

__NDPPD_NS_BEGIN

std::map<std::string, strong_ptr<iface> > iface::_map;

std::vector<struct pollfd> iface::_pollfds;

iface::iface() :
   _ifd(-1), _pfd(-1)
{
}

iface::~iface()
{
   DBG("iface::~iface()");
}

strong_ptr<iface> iface::open_pfd(const std::string& name)
{
   int fd;

   std::map<std::string, strong_ptr<iface> >::iterator it = _map.find(name);

   strong_ptr<iface> ifa;

   if(it != _map.end())
   {
      if(it->second->_pfd >= 0)
         return it->second;

      ifa = it->second;
   }
   else
   {
      // We need an _ifs, so let's set one up.
      ifa = open_ifd(name);
   }

   if(ifa.is_null())
      return strong_ptr<iface>();

   // Create a socket.

   if((fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_IPV6))) < 0)
   {
      ERR("Unable to create socket");
      return strong_ptr<iface>();
   }

   // Bind to the specified interface.

   struct ifreq ifr;

   memset(&ifr, 0, sizeof(ifr));
   strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
   ifr.ifr_name[IFNAMSIZ - 1] = '\0';

   if(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0)
   {
      close(fd);
      ERR("Failed to bind to interface '%s'", name.c_str());
      return strong_ptr<iface>();
   }

   // Switch to non-blocking mode.

   int on = 1;

   if(ioctl(fd, FIONBIO, (char *)&on) < 0)
   {
      close(fd);
      ERR("Failed to switch to non-blocking on interface '%s'", name.c_str());
      return strong_ptr<iface>();
   }

   // Set up filter.

   struct sock_fprog fprog;

   static const struct sock_filter filter[] =
   {
      // Load the ether_type.
      BPF_STMT(BPF_LD | BPF_H | BPF_ABS,
         offsetof(struct ether_header, ether_type)),
      // Bail if it's *not* ETHERTYPE_IPV6.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETHERTYPE_IPV6, 0, 5),
      // Load the next header type.
      BPF_STMT(BPF_LD | BPF_B | BPF_ABS,
         sizeof(struct ether_header) + offsetof(struct ip6_hdr, ip6_nxt)),
      // Bail if it's *not* IPPROTO_ICMPV6.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_ICMPV6, 0, 3),
      // Load the ICMPv6 type.
      BPF_STMT(BPF_LD | BPF_B | BPF_ABS,
         sizeof(struct ether_header) + sizeof(ip6_hdr) + offsetof(struct icmp6_hdr, icmp6_type)),
      // Bail if it's *not* ND_NEIGHBOR_SOLICIT.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ND_NEIGHBOR_SOLICIT, 0, 1),
      // Keep packet.
      BPF_STMT(BPF_RET | BPF_K, -1),
      // Drop packet.
      BPF_STMT(BPF_RET | BPF_K, 0)
   };

   fprog.filter = (struct sock_filter *)filter;
   fprog.len    = 8;

   if(setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof(fprog)) < 0)
   {
      ERR("Failed to set filter");
      return strong_ptr<iface>();
   }

   // Set up an instance of 'iface'.

   ifa->_pfd = fd;

   fixup_pollfds();

   return ifa;
}

strong_ptr<iface> iface::open_ifd(const std::string& name)
{
   int fd;

   std::map<std::string, strong_ptr<iface> >::iterator it = _map.find(name);

   if((it != _map.end()) && it->second->_ifd)
      return it->second;

   // Create a socket.

   if((fd = socket(PF_INET6, SOCK_RAW, IPPROTO_ICMPV6)) < 0)
   {
      ERR("Unable to create socket");
      return strong_ptr<iface>();
   }

   // Bind to the specified interface.

   struct ifreq ifr;

   memset(&ifr, 0, sizeof(ifr));
   strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
   ifr.ifr_name[IFNAMSIZ - 1] = '\0';

   if(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0)
   {
      close(fd);
      ERR("Failed to bind to interface '%s'", name.c_str());
      return strong_ptr<iface>();
   }

   // Detect the link-layer address.

   memset(&ifr, 0, sizeof(ifr));
   strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
   ifr.ifr_name[IFNAMSIZ - 1] = '\0';

   if(ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
   {
      close(fd);
      ERR("Failed to detect link-layer address for interface '%s'", name.c_str());
      return strong_ptr<iface>();
   }

   DBG("fd=%d, hwaddr=%s", fd, ether_ntoa((const struct ether_addr *)&ifr.ifr_hwaddr.sa_data));

   // Set max hops.

   int hops = 255;

   if(setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0)
   {
      close(fd);
      ERR("iface::open_ifd() failed IPV6_MULTICAST_HOPS");
      return strong_ptr<iface>();
   }

   // Switch to non-blocking mode.

   int on = 1;

   if(ioctl(fd, FIONBIO, (char *)&on) < 0)
   {
      close(fd);
      ERR("Failed to switch to non-blocking on interface '%s'", name.c_str());
      return strong_ptr<iface>();
   }

   // Set up filter.

   struct icmp6_filter filter;
   ICMP6_FILTER_SETBLOCKALL(&filter);
   ICMP6_FILTER_SETPASS(ND_NEIGHBOR_ADVERT, &filter);

   if(setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) < 0)
   {
      ERR("Failed to set filter");
      return strong_ptr<iface>();
   }

   // Set up an instance of 'iface'.

   strong_ptr<iface> ifa;

   if(it == _map.end())
   {
      ifa = new iface();

      ifa->_name = name;
      ifa->_ptr  = ifa;

      _map[name] = ifa;
   }
   else
   {
      ifa = it->second;
   }

   ifa->_ifd = fd;

   memcpy(&ifa->hwaddr, ifr.ifr_hwaddr.sa_data, sizeof(struct ether_addr));

   fixup_pollfds();

   return ifa;
}

ssize_t iface::read(int fd, address& saddr, uint8_t *msg, size_t size)
{
   struct sockaddr_in6 saddr_tmp;
   struct msghdr mhdr;
   struct iovec iov;
   char cbuf[256];
   int len;

   if(!msg || (size < 0))
      return -1;

   iov.iov_len = size;
   iov.iov_base = (caddr_t)msg;

   memset(&mhdr, 0, sizeof(mhdr));
   mhdr.msg_name = (caddr_t)&saddr_tmp;
   mhdr.msg_namelen = sizeof(saddr_tmp);
   mhdr.msg_iov = &iov;
   mhdr.msg_iovlen = 1;

   if((len = recvmsg(fd, &mhdr, 0)) < 0)
      return -1;

   if(len < sizeof(struct icmp6_hdr))
      return -1;

   saddr = saddr_tmp.sin6_addr;

   DBG("iface::read() saddr=%s, len=%d", saddr.to_string().c_str(), len);

   return len;
}

ssize_t iface::write(int fd, const address& daddr, const uint8_t *msg, size_t size)
{
   struct sockaddr_in6 daddr_tmp;
   struct msghdr mhdr;
   struct iovec iov;

   memset(&daddr_tmp, 0, sizeof(struct sockaddr_in6));
   daddr_tmp.sin6_family = AF_INET6;
   daddr_tmp.sin6_port   = htons(IPPROTO_ICMPV6); // Needed?
   memcpy(&daddr_tmp.sin6_addr, &daddr.const_addr(), sizeof(struct in6_addr));

   iov.iov_len = size;
   iov.iov_base = (caddr_t)msg;

   memset(&mhdr, 0, sizeof(mhdr));
   mhdr.msg_name = (caddr_t)&daddr_tmp;
   mhdr.msg_namelen = sizeof(sockaddr_in6);
   mhdr.msg_iov = &iov;
   mhdr.msg_iovlen = 1;

   DBG("iface::write() daddr=%s, len=%d", daddr.to_string().c_str(), size);

   int len;

   if((len = sendmsg(fd, &mhdr, 0)) < 0)
      return -1;

   return len;
}

ssize_t iface::read_solicit(address& saddr, address& daddr, address& taddr)
{
   uint8_t msg[256];
   ssize_t len;

   if((len = read(_pfd, saddr, msg, sizeof(msg))) < 0)
      return -1;

   struct ip6_hdr *ip6h =
        (struct ip6_hdr *)(msg + ETH_HLEN);

   struct icmp6_hdr *icmph =
        (struct icmp6_hdr *)(msg + ETH_HLEN + sizeof( struct ip6_hdr));

   struct nd_neighbor_solicit  *ns =
      (struct nd_neighbor_solicit *)(msg + ETH_HLEN + sizeof( struct ip6_hdr));

   taddr = ns->nd_ns_target;
   daddr = ip6h->ip6_dst;
   saddr = ip6h->ip6_src;

   DBG("iface::read_solicit() saddr=%s, daddr=%s, taddr=%s, len=%d",
      daddr.to_string().c_str(), saddr.to_string().c_str(),
      taddr.to_string().c_str(), len);

   return len;
}

ssize_t iface::write_solicit(const address& taddr)
{
   char buf[128];

   memset(buf, 0, sizeof(buf));

   struct nd_neighbor_solicit *ns =
      (struct nd_neighbor_solicit *)&buf[0];

   struct nd_opt_hdr *opt =
      (struct nd_opt_hdr *)&buf[sizeof(struct nd_neighbor_solicit)];

   opt->nd_opt_type = ND_OPT_SOURCE_LINKADDR;
   opt->nd_opt_len  = 1;

   ns->nd_ns_type   = ND_NEIGHBOR_SOLICIT;

   memcpy(&ns->nd_ns_target, &taddr.const_addr(), sizeof(struct in6_addr));

   memcpy(buf + sizeof(struct nd_neighbor_solicit) + sizeof(struct nd_opt_hdr), &hwaddr, 6);

   // FIXME: Alright, I'm lazy.
   static address multicast("ff02::1:ff00:0000");

   address daddr;

   daddr = multicast;

   daddr.addr().s6_addr[13] = taddr.const_addr().s6_addr[13];
   daddr.addr().s6_addr[14] = taddr.const_addr().s6_addr[14];
   daddr.addr().s6_addr[15] = taddr.const_addr().s6_addr[15];

   DBG("iface::write_solicit() taddr=%s, daddr=%s",
       taddr.to_string().c_str(), daddr.to_string().c_str());

   return write(_ifd, daddr, (uint8_t *)buf, sizeof(struct nd_neighbor_solicit) + sizeof(struct nd_opt_hdr) + 6);
}

ssize_t iface::write_advert(const address& daddr, const address& taddr)
{
   char buf[128];

   memset(buf, 0, sizeof(buf));

   struct nd_neighbor_advert *na =
      (struct nd_neighbor_advert *)&buf[0];

   struct nd_opt_hdr *opt =
      (struct nd_opt_hdr *)&buf[sizeof(struct nd_neighbor_advert)];

   opt->nd_opt_type         = ND_OPT_TARGET_LINKADDR;
   opt->nd_opt_len          = 1;

   na->nd_na_type           = ND_NEIGHBOR_ADVERT;
   na->nd_na_flags_reserved = ND_NA_FLAG_SOLICITED | ND_NA_FLAG_ROUTER;

   memcpy(&na->nd_na_target, &taddr.const_addr(), sizeof(struct in6_addr));

   memcpy(buf + sizeof(struct nd_neighbor_advert) + sizeof(struct nd_opt_hdr), &hwaddr, 6);

   DBG("iface::write_advert() daddr=%s, taddr=%s",
       daddr.to_string().c_str(), taddr.to_string().c_str());

   return write(_ifd, daddr, (uint8_t *)buf, sizeof(struct nd_neighbor_advert) +
      sizeof(struct nd_opt_hdr) + 6);
}

ssize_t iface::read_advert(address& saddr, address& taddr)
{
   uint8_t msg[256];
   ssize_t len;

   if((len = read(_ifd, saddr, msg, sizeof(msg))) < 0)
      return -1;

   if(((struct icmp6_hdr *)msg)->icmp6_type != ND_NEIGHBOR_ADVERT)
      return -1;

   taddr = ((struct nd_neighbor_solicit *)msg)->nd_ns_target;

   DBG("iface::read_advert() saddr=%s, taddr=%s, len=%d",
      saddr.to_string().c_str(), taddr.to_string().c_str(), len);

   return len;
}

void iface::fixup_pollfds()
{
   _pollfds.resize(_map.size() * 2);

   int i = 0;

   DBG("iface::fixup_pollfds() _map.size()=%d", _map.size());

   for(std::map<std::string, strong_ptr<iface> >::iterator it = _map.begin();
      it != _map.end(); it++)
   {
      _pollfds[i].fd      = it->second->_ifd;
      _pollfds[i].events  = POLLIN;
      _pollfds[i].revents = 0;
      i++;

      _pollfds[i].fd      = it->second->_pfd;
      _pollfds[i].events  = POLLIN;
      _pollfds[i].revents = 0;
      i++;
   }
}

void iface::remove_session(const strong_ptr<session>& se)
{
   _sessions.remove(se);
}

void iface::add_session(const strong_ptr<session>& se)
{
   _sessions.push_back(se);
}

int iface::poll_all()
{
   if(_pollfds.size() == 0)
   {
      ::sleep(1);
      return 0;
   }

   assert(_pollfds.size() == _map.size() * 2);

   int len;

   if((len = ::poll(&_pollfds[0], _pollfds.size(), 50)) < 0)
      return -1;

   if(len == 0)
      return 0;

   std::map<std::string, strong_ptr<iface> >::iterator i_it = _map.begin();

   int i = 0;

   for(std::vector<struct pollfd>::iterator f_it = _pollfds.begin();
       f_it != _pollfds.end(); f_it++)
   {
      assert(i_it != _map.end());

      if(i && !(i % 2))
         i_it++;

      bool is_pfd = i++ % 2;

      if(!(f_it->revents & POLLIN))
         continue;

      strong_ptr<iface> ifa = i_it->second;

      address saddr, daddr, taddr;

      if(is_pfd)
      {
         if(ifa->read_solicit(saddr, daddr, taddr) < 0)
         {
            ERR("Failed to read from interface '%s'", ifa->_name.c_str());
            continue;
         }

         ifa->_pr->handle_solicit(saddr, daddr, taddr);
      }
      else
      {
         if(ifa->read_advert(saddr, taddr) < 0)
         {
            ERR("Failed to read from interface '%s'", ifa->_name.c_str());
            continue;
         }

         for(std::list<strong_ptr<session> >::iterator s_it = ifa->_sessions.begin();
             s_it != ifa->_sessions.end(); s_it++)
         {
            if(((*s_it)->taddr() == taddr) && ((*s_it)->status() == session::WAITING))
            {
               (*s_it)->handle_advert();
               break;
            }
         }
      }
   }

   return 0;
}

int iface::allmulti(int state)
{
   struct ifreq ifr;

   DBG("iface::allmulti() state=%d, _name=\"%s\"",
      state, _name.c_str());

   state = !!state;

   strncpy(ifr.ifr_name, _name.c_str(), IFNAMSIZ);

   if(ioctl(_pfd, SIOCGIFFLAGS, &ifr) < 0)
      return -1;

   int old_state = !!(ifr.ifr_flags & IFF_ALLMULTI);

   if(state == old_state)
      return old_state;

   if(state)
      ifr.ifr_flags |= IFF_ALLMULTI;
   else
      ifr.ifr_flags &= ~IFF_ALLMULTI;

   if(ioctl(_pfd, SIOCSIFFLAGS, &ifr) < 0)
      return -1;

   return old_state;
}

const std::string& iface::name() const
{
   return _name;
}

void iface::pr(const strong_ptr<proxy>& pr)
{
   _pr = pr;
}

const strong_ptr<proxy>& iface::pr() const
{
   return _pr;
}

__NDPPD_NS_END