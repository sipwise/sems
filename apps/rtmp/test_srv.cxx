/*
 * Copyright (C) 2011 Raphael Coeffic
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>

#include "RtmpServer.h"
#include <librtmp/log.h>

void print_usage(char* prog)
{
  fprintf(stderr,"%s [-z]\n",prog);
}


int main(int argc, char** argv)
{
  RTMP_LogSetLevel(RTMP_LOGDEBUG);

  if(argc>2){
    fprintf(stderr,"Too many arguments");
    goto error;
  }
  else if(argc>1){
    if((strlen(argv[1]) != sizeof("-z")-1) ||
       strncmp(argv[1],"-z",sizeof("-z")-1)) {
      fprintf(stderr,"Unknown argument '%s'\n",argv[1]);
      goto error;
    }
    RTMP_LogSetLevel(RTMP_LOGALL);
  }

  RtmpServer::instance()->start();
  RtmpServer::instance()->join();

  return 0;

 error:
  print_usage(argv[0]);
  return -1;
}
