/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2007 Nokia Corporation. All rights reserved.
 *  Contact: Rémi Denis-Courmont
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Rémi Denis-Courmont, Nokia
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#ifndef SOL_IP
# define SOL_IP IPPROTO_IP
# define SOL_IPV6 IPPROTO_IPV6
#endif

#ifndef IPV6_RECVPKTINFO
# define IPV6_RECVPKTINFO IPV6_PKTINFO
#endif

/** Default port for STUN binding discovery */
#define IPPORT_STUN  3478

#include "stun/stunagent.h"
#include "stund.h"

static const uint16_t known_attributes[] =  {
  0
};

/**
 * Creates a listening socket
 */
int listen_socket (int fam, int type, int proto, uint16_t port)
{
  int yes = 1;
  int fd = socket (fam, type, proto);
  if (fd == -1)
  {
    perror ("Error opening IP port");
    return -1;
  }
  if (fd < 3)
    goto error;

  struct sockaddr_storage addr;
  memset (&addr, 0, sizeof (addr));
  addr.ss_family = fam;
#ifdef HAVE_SA_LEN
  addr.ss_len = sizeof (addr);
#endif

  switch (fam)
  {
    case AF_INET:
      ((struct sockaddr_in *)&addr)->sin_port = port;
      break;

    case AF_INET6:
#ifdef IPV6_V6ONLY
      setsockopt (fd, SOL_IPV6, IPV6_V6ONLY, &yes, sizeof (yes));
#endif
      ((struct sockaddr_in6 *)&addr)->sin6_port = port;
      break;
  }

  if (bind (fd, (struct sockaddr *)&addr, sizeof (addr)))
  {
    perror ("Error opening IP port");
    goto error;
  }

  if ((type == SOCK_DGRAM) || (type == SOCK_RAW))
  {
    switch (fam)
    {
      case AF_INET:
        setsockopt (fd, SOL_IP, IP_PKTINFO, &yes, sizeof (yes));
#ifdef IP_RECVERR
        setsockopt (fd, SOL_IP, IP_RECVERR, &yes, sizeof (yes));
#endif
        break;

      case AF_INET6:
        setsockopt (fd, SOL_IPV6, IPV6_RECVPKTINFO, &yes,
                    sizeof (yes));
#ifdef IPV6_RECVERR
        setsockopt (fd, SOL_IPV6, IPV6_RECVERR, &yes, sizeof (yes));
#endif
        break;
    }
  }
  else
  {
    if (listen (fd, INT_MAX))
    {
      perror ("Error opening IP port");
      goto error;
    }
  }

  return fd;

error:
  close (fd);
  return -1;
}


/** Dequeue error from a socket if applicable */
static int recv_err (int fd)
{
#ifdef MSG_ERRQUEUE
  struct msghdr hdr;
  memset (&hdr, 0, sizeof (hdr));
  return recvmsg (fd, &hdr, MSG_ERRQUEUE) >= 0;
#endif
}


/** Receives a message or dequeues an error from a socket */
ssize_t recv_safe (int fd, struct msghdr *msg)
{
  ssize_t len = recvmsg (fd, msg, 0);
  if (len == -1)
    recv_err (fd);
  else
  if (msg->msg_flags & MSG_TRUNC)
  {
    errno = EMSGSIZE;
    return -1;
  }

  return len;
}


/** Sends a message through a socket */
ssize_t send_safe (int fd, const struct msghdr *msg)
{
  ssize_t len;

  do
    len = sendmsg (fd, msg, 0);
  while ((len == -1) && (recv_err (fd) == 0));

  return len;
}


static int dgram_process (int sock, StunAgent *agent)
{
  struct sockaddr_storage addr;
  uint8_t buf[STUN_MAX_MESSAGE_SIZE];
  char ctlbuf[CMSG_SPACE (sizeof (struct in6_pktinfo))];
  struct iovec iov = { buf, sizeof (buf) };
  StunMessage request;
  StunMessage response;
  StunValidationStatus validation;

  struct msghdr mh =
  {
    .msg_name = (struct sockaddr *)&addr,
    .msg_namelen = sizeof (addr),
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = ctlbuf,
    .msg_controllen = sizeof (ctlbuf)
  };

  size_t len = recv_safe (sock, &mh);
  if (len == (size_t)-1)
    return -1;

  validation = stun_agent_validate (agent, &request, buf, len, NULL, 0);

  /* Unknown attributes */
  if (validation == STUN_VALIDATION_UNKNOWN_REQUEST_ATTRIBUTE)
  {
    stun_agent_build_unknown_attributes_error (agent, &response, buf,
        sizeof (buf), &request);
    goto send_buf;
  }

  /* Mal-formatted packets */
  if (validation != STUN_VALIDATION_SUCCESS ||
      stun_message_get_class (&request) != STUN_REQUEST) {
    return -1;
  }

  switch (stun_message_get_method (&request))
  {
    case STUN_BINDING:
      stun_agent_init_response (agent, &response, buf, sizeof (buf), &request);
      if (stun_has_cookie (&request))
        stun_message_append_xor_addr (&response,
                              STUN_ATTRIBUTE_XOR_MAPPED_ADDRESS,
                              mh.msg_name, mh.msg_namelen);
      else
         stun_message_append_addr (&response, STUN_ATTRIBUTE_MAPPED_ADDRESS,
                          mh.msg_name, mh.msg_namelen);
      break;

    default:
      stun_agent_init_error (agent, &response, buf, sizeof (buf),
          &request, STUN_ERROR_BAD_REQUEST);
  }

  iov.iov_len = stun_agent_finish_message (agent, &response, NULL, 0);
send_buf:

  len = send_safe (sock, &mh);
  return (len < iov.iov_len) ? -1 : 0;
}


static int run (int family, int protocol, unsigned port)
{
  StunAgent agent;
  int sock = listen_socket (family, SOCK_DGRAM, protocol, htons (port));
  if (sock == -1)
    return -1;


  stun_agent_init (&agent, known_attributes,
      STUN_COMPATIBILITY_3489BIS, STUN_AGENT_USAGE_ADD_SERVER);

  for (;;)
    dgram_process (sock, &agent);
}


/* Pretty useless dummy signal handler...
 * But calling exit() is needed for gcov to work properly. */
static void exit_handler (int signum)
{
  (void)signum;
  exit (0);
}


int main (int argc, char *argv[])
{
  int family = AF_INET;
  unsigned port = IPPORT_STUN;

  for (;;)
  {
    int c = getopt (argc, argv, "46");
    if (c == EOF)
      break;

    switch (c)
    {
      case '4':
        family = AF_INET;
        break;

      case '6':
        family = AF_INET6;
        break;
    }
  }

  if (optind < argc)
    port = atoi (argv[optind++]);

  signal (SIGINT, exit_handler);
  signal (SIGTERM, exit_handler);
  return run (family, IPPROTO_UDP, port) ? EXIT_FAILURE : EXIT_SUCCESS;
}