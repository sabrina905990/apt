// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: http.cc,v 1.4 1998/11/05 07:21:48 jgg Exp $
/* ######################################################################

   HTTP Aquire Method - This is the HTTP aquire method for APT.
   
   It uses HTTP/1.1 and many of the fancy options there-in, such as
   pipelining, range, if-range and so on. It accepts on the command line
   a list of url destination pairs and writes to stdout the status of the
   operation as defined in the APT method spec.
   
   It is based on a doubly buffered select loop. All the requests are 
   fed into a single output buffer that is constantly fed out the 
   socket. This provides ideal pipelining as in many cases all of the
   requests will fit into a single packet. The input socket is buffered 
   the same way and fed into the fd for the file.
   
   This double buffering provides fairly substantial transfer rates,
   compared to wget the http method is about 4% faster. Most importantly,
   when HTTP is compared with FTP as a protocol the speed difference is
   huge. In tests over the internet from two sites to llug (via ATM) this
   program got 230k/s sustained http transfer rates. FTP on the other 
   hand topped out at 170k/s. That combined with the time to setup the
   FTP connection makes HTTP a vastly superior protocol.
      
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <apt-pkg/fileutl.h>
#include <apt-pkg/acquire-method.h>
#include <apt-pkg/error.h>
#include <apt-pkg/md5.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>

// Internet stuff
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "http.h"
									/*}}}*/

string HttpMethod::FailFile;
int HttpMethod::FailFd = -1;
time_t HttpMethod::FailTime = 0;

// CircleBuf::CircleBuf - Circular input buffer				/*{{{*/
// ---------------------------------------------------------------------
/* */
CircleBuf::CircleBuf(unsigned long Size) : Size(Size), MD5(0)
{
   Buf = new unsigned char[Size];
   Reset();
}
									/*}}}*/
// CircleBuf::Reset - Reset to the default state			/*{{{*/
// ---------------------------------------------------------------------
/* */
void CircleBuf::Reset()
{
   InP = 0;
   OutP = 0;
   StrPos = 0;
   MaxGet = (unsigned int)-1;
   OutQueue = string();
   if (MD5 != 0)
   {
      delete MD5;
      MD5 = new MD5Summation;
   }   
};
									/*}}}*/
// CircleBuf::Read - Read from a FD into the circular buffer		/*{{{*/
// ---------------------------------------------------------------------
/* This fills up the buffer with as much data as is in the FD, assuming it
   is non-blocking.. */
bool CircleBuf::Read(int Fd)
{
   while (1)
   {
      // Woops, buffer is full
      if (InP - OutP == Size)
	 return true;
      
      // Write the buffer segment
      int Res;
      Res = read(Fd,Buf + (InP%Size),LeftRead());
      
      if (Res == 0)
	 return false;
      if (Res < 0)
      {
	 if (errno == EAGAIN)
	    return true;
	 return false;
      }

      if (InP == 0)
	 gettimeofday(&Start,0);
      InP += Res;
   }
}
									/*}}}*/
// CircleBuf::Read - Put the string into the buffer			/*{{{*/
// ---------------------------------------------------------------------
/* This will hold the string in and fill the buffer with it as it empties */
bool CircleBuf::Read(string Data)
{
   OutQueue += Data;
   FillOut();
   return true;
}
									/*}}}*/
// CircleBuf::FillOut - Fill the buffer from the output queue		/*{{{*/
// ---------------------------------------------------------------------
/* */
void CircleBuf::FillOut()
{
   if (OutQueue.empty() == true)
      return;
   while (1)
   {
      // Woops, buffer is full
      if (InP - OutP == Size)
	 return;
      
      // Write the buffer segment
      unsigned long Sz = LeftRead();
      if (OutQueue.length() - StrPos < Sz)
	 Sz = OutQueue.length() - StrPos;
      memcpy(Buf + (InP%Size),OutQueue.begin() + StrPos,Sz);
      
      // Advance
      StrPos += Sz;
      InP += Sz;
      if (OutQueue.length() == StrPos)
      {
	 StrPos = 0;
	 OutQueue = "";
	 return;
      }
   }
}
									/*}}}*/
// CircleBuf::Write - Write from the buffer into a FD			/*{{{*/
// ---------------------------------------------------------------------
/* This empties the buffer into the FD. */
bool CircleBuf::Write(int Fd)
{
   while (1)
   {
      FillOut();
      
      // Woops, buffer is empty
      if (OutP == InP)
	 return true;
      
      if (OutP == MaxGet)
	 return true;
      
      // Write the buffer segment
      int Res;
      Res = write(Fd,Buf + (OutP%Size),LeftWrite());

      if (Res == 0)
	 return false;
      if (Res < 0)
      {
	 if (errno == EAGAIN)
	    return true;
	 
	 return false;
      }
      
      if (MD5 != 0)
	 MD5->Add(Buf + (OutP%Size),Res);
      
      OutP += Res;
   }
}
									/*}}}*/
// CircleBuf::WriteTillEl - Write from the buffer to a string		/*{{{*/
// ---------------------------------------------------------------------
/* This copies till the first empty line */
bool CircleBuf::WriteTillEl(string &Data,bool Single)
{
   // We cheat and assume it is unneeded to have more than one buffer load
   for (unsigned long I = OutP; I < InP; I++)
   {      
      if (Buf[I%Size] != '\n')
	 continue;
      for (I++; I < InP && Buf[I%Size] == '\r'; I++);
      
      if (Single == false)
      {
	 if (Buf[I%Size] != '\n')
	    continue;
	 for (I++; I < InP && Buf[I%Size] == '\r'; I++);
      }
      
      if (I > InP)
	 I = InP;
      
      Data = "";
      while (OutP < I)
      {
	 unsigned long Sz = LeftWrite();
	 if (Sz == 0)
	    return false;
	 if (I - OutP < LeftWrite())
	    Sz = I - OutP;
	 Data += string((char *)(Buf + (OutP%Size)),Sz);
	 OutP += Sz;
      }
      return true;
   }      
   return false;
}
									/*}}}*/
// CircleBuf::Stats - Print out stats information			/*{{{*/
// ---------------------------------------------------------------------
/* */
void CircleBuf::Stats()
{
   if (InP == 0)
      return;
   
   struct timeval Stop;
   gettimeofday(&Stop,0);
/*   float Diff = Stop.tv_sec - Start.tv_sec + 
             (float)(Stop.tv_usec - Start.tv_usec)/1000000;
   clog << "Got " << InP << " in " << Diff << " at " << InP/Diff << endl;*/
}
									/*}}}*/

// ServerState::ServerState - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
ServerState::ServerState(URI Srv,HttpMethod *Owner) : Owner(Owner),
                        In(64*1024), Out(1*1024),
                        ServerName(Srv)
{
   Reset();
}
									/*}}}*/
// ServerState::Open - Open a connection to the server			/*{{{*/
// ---------------------------------------------------------------------
/* This opens a connection to the server. */
string LastHost;
in_addr LastHostA;
bool ServerState::Open()
{
   // Use the already open connection if possible.
   if (ServerFd != -1)
      return true;
   
   Close();
   In.Reset();
   Out.Reset();

   // Determine the proxy setting
   string DefProxy = _config->Find("Acquire::http::Proxy",getenv("http_proxy"));
   string SpecificProxy = _config->Find("Acquire::http::Proxy::" + ServerName.Host);
   if (SpecificProxy.empty() == false)
   {
      if (SpecificProxy == "DIRECT")
	 Proxy = "";
      else
	 Proxy = SpecificProxy;
   }   
   else
      Proxy = DefProxy;

   // Determine what host and port to use based on the proxy settings
   int Port = 80;
   string Host;   
   if (Proxy.empty() == true)
   {
      if (ServerName.Port != 0)
	 Port = ServerName.Port;
      Host = ServerName.Host;
   }
   else
   {
      if (Proxy.Port != 0)
	 Port = Proxy.Port;
      Host = Proxy.Host;
   }
   
   /* We used a cached address record.. Yes this is against the spec but
      the way we have setup our rotating dns suggests that this is more
      sensible */
   if (LastHost != Host)
   {
      Owner->Status("Connecting to %s",Host.c_str());

      // Lookup the host
      hostent *Addr = gethostbyname(Host.c_str());
      if (Addr == 0)
	 return _error->Error("Could not resolve '%s'",Host.c_str());
      LastHost = Host;
      LastHostA = *(in_addr *)(Addr->h_addr_list[0]);
   }
   
   Owner->Status("Connecting to %s (%s)",Host.c_str(),inet_ntoa(LastHostA));
   
   // Get a socket
   if ((ServerFd = socket(AF_INET,SOCK_STREAM,0)) < 0)
      return _error->Errno("socket","Could not create a socket");
   
   // Connect to the server
   struct sockaddr_in server;
   server.sin_family = AF_INET;
   server.sin_port = htons(Port);
   server.sin_addr = LastHostA;
   if (connect(ServerFd,(sockaddr *)&server,sizeof(server)) < 0)
      return _error->Errno("socket","Could not create a socket");

   SetNonBlock(ServerFd,true);
   return true;
}
									/*}}}*/
// ServerState::Close - Close a connection to the server		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ServerState::Close()
{
   close(ServerFd);
   ServerFd = -1;
   return true;
}
									/*}}}*/
// ServerState::RunHeaders - Get the headers before the data		/*{{{*/
// ---------------------------------------------------------------------
/* Returns 0 if things are OK, 1 if an IO error occursed and 2 if a header
   parse error occured */
int ServerState::RunHeaders()
{
   State = Header;
   
   Owner->Status("Waiting for file");

   Major = 0; 
   Minor = 0; 
   Result = 0; 
   Size = 0; 
   StartPos = 0;
   Encoding = Closes;
   HaveContent = false;
   time(&Date);

   do
   {
      string Data;
      if (In.WriteTillEl(Data) == false)
	 continue;
      
      for (string::const_iterator I = Data.begin(); I < Data.end(); I++)
      {
	 string::const_iterator J = I;
	 for (; J != Data.end() && *J != '\n' && *J != '\r';J++);
	 if (HeaderLine(string(I,J-I)) == false)
	    return 2;
	 I = J;
      }
      return 0;
   }
   while (Owner->Go(false,this) == true);

   return 1;
}
									/*}}}*/
// ServerState::RunData - Transfer the data from the socket		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ServerState::RunData()
{
   State = Data;
   
   // Chunked transfer encoding is fun..
   if (Encoding == Chunked)
   {
      while (1)
      {
	 // Grab the block size
	 bool Last = true;
	 string Data;
	 In.Limit(-1);
	 do
	 {
	    if (In.WriteTillEl(Data,true) == true)
	       break;
	 }
	 while ((Last = Owner->Go(false,this)) == true);

	 if (Last == false)
	    return false;
	 	 
	 // See if we are done
	 unsigned long Len = strtol(Data.c_str(),0,16);
	 if (Len == 0)
	 {
	    In.Limit(-1);
	    
	    // We have to remove the entity trailer
	    Last = true;
	    do
	    {
	       if (In.WriteTillEl(Data,true) == true && Data.length() <= 2)
		  break;
	    }
	    while ((Last = Owner->Go(false,this)) == true);
	    if (Last == false)
	       return false;
	    return true;
	 }
	 
	 // Transfer the block
	 In.Limit(Len);
	 while (Owner->Go(true,this) == true)
	    if (In.IsLimit() == true)
	       break;
	 
	 // Error
	 if (In.IsLimit() == false)
	    return false;
	 
	 // The server sends an extra new line before the next block specifier..
	 In.Limit(-1);
	 Last = true;
	 do
	 {
	    if (In.WriteTillEl(Data,true) == true)
	       break;
	 }
	 while ((Last = Owner->Go(false,this)) == true);
	 if (Last == false)
	    return false;
      }
   }
   else
   {
      /* Closes encoding is used when the server did not specify a size, the
         loss of the connection means we are done */
      if (Encoding == Closes)
	 In.Limit(-1);
      else
	 In.Limit(Size - StartPos);
      
      // Just transfer the whole block.
      do
      {
	 if (In.IsLimit() == false)
	    continue;
	 
	 In.Limit(-1);
	 return true;
      }
      while (Owner->Go(true,this) == true);
   }

   return Owner->Flush(this);
}
									/*}}}*/
// ServerState::HeaderLine - Process a header line			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ServerState::HeaderLine(string Line)
{
   if (Line.empty() == true)
      return true;
   
   // The http server might be trying to do something evil.
   if (Line.length() >= MAXLEN)
      return _error->Error("Got a single header line over %u chars",MAXLEN);

   string::size_type Pos = Line.find(' ');
   if (Pos == string::npos || Pos+1 > Line.length())
      return _error->Error("Bad header line");
   
   string Tag = string(Line,0,Pos);
   string Val = string(Line,Pos+1);

   if (stringcasecmp(Tag.begin(),Tag.begin()+4,"HTTP") == 0)
   {
      // Evil servers return no version
      if (Line[4] == '/')
      {
	 if (sscanf(Line.c_str(),"HTTP/%u.%u %u %[^\n]",&Major,&Minor,
		    &Result,Code) != 4)
	    return _error->Error("The http server sent an invalid reply header");
      }
      else
      {
	 Major = 0;
	 Minor = 9;
	 if (sscanf(Line.c_str(),"HTTP %u %[^\n]",&Result,Code) != 2)
	    return _error->Error("The http server sent an invalid reply header");
      }
      
      return true;
   }      
      
   if (stringcasecmp(Tag,"Content-Length:") == 0)
   {
      if (Encoding == Closes)
	 Encoding = Stream;
      HaveContent = true;
      
      // The length is already set from the Content-Range header
      if (StartPos != 0)
	 return true;
      
      if (sscanf(Val.c_str(),"%lu",&Size) != 1)
	 return _error->Error("The http server sent an invalid Content-Length header");
      return true;
   }

   if (stringcasecmp(Tag,"Content-Type:") == 0)
   {
      HaveContent = true;
      return true;
   }
   
   if (stringcasecmp(Tag,"Content-Range:") == 0)
   {
      HaveContent = true;
      
      if (sscanf(Val.c_str(),"bytes %lu-%*u/%lu",&StartPos,&Size) != 2)
	 return _error->Error("The http server sent an invalid Content-Range header");
      if ((unsigned)StartPos > Size)
	 return _error->Error("This http server has broken range support");
      return true;
   }
   
   if (stringcasecmp(Tag,"Transfer-Encoding:") == 0)
   {
      HaveContent = true;
      if (stringcasecmp(Val,"chunked") == 0)
	 Encoding = Chunked;
      
      return true;
   }

   if (stringcasecmp(Tag,"Last-Modified:") == 0)
   {
      if (StrToTime(Val,Date) == false)
	 return _error->Error("Unknown date format");
      return true;
   }

   return true;
}
									/*}}}*/

// HttpMethod::SendReq - Send the HTTP request				/*{{{*/
// ---------------------------------------------------------------------
/* This places the http request in the outbound buffer */
void HttpMethod::SendReq(FetchItem *Itm,CircleBuf &Out)
{
   URI Uri = Itm->Uri;
   
   // The HTTP server expects a hostname with a trailing :port
   char Buf[300];
   string ProperHost = Uri.Host;
   if (Uri.Port != 0)
   {
      sprintf(Buf,":%u",Uri.Port);
      ProperHost += Buf;
   }   
      
   /* Build the request. We include a keep-alive header only for non-proxy
      requests. This is to tweak old http/1.0 servers that do support keep-alive
      but not HTTP/1.1 automatic keep-alive. Doing this with a proxy server 
      will glitch HTTP/1.0 proxies because they do not filter it out and 
      pass it on, HTTP/1.1 says the connection should default to keep alive
      and we expect the proxy to do this */
   if (Proxy.empty() == true)
      sprintf(Buf,"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n",
	      Uri.Path.c_str(),ProperHost.c_str());
   else
      sprintf(Buf,"GET %s HTTP/1.1\r\nHost: %s\r\n",
	      Itm->Uri.c_str(),ProperHost.c_str());
   string Req = Buf;

   // Check for a partial file
   struct stat SBuf;
   if (stat(Itm->DestFile.c_str(),&SBuf) >= 0 && SBuf.st_size > 0)
   {
      // In this case we send an if-range query with a range header
      sprintf(Buf,"Range: bytes=%li-\r\nIf-Range: %s\r\n",SBuf.st_size - 1,
	      TimeRFC1123(SBuf.st_mtime).c_str());
      Req += Buf;
   }
   else
   {
      if (Itm->LastModified != 0)
      {
	 sprintf(Buf,"If-Modified-Since: %s\r\n",TimeRFC1123(Itm->LastModified).c_str());
	 Req += Buf;
      }
   }

/*   if (ProxyAuth.empty() == false)
      Req += string("Proxy-Authorization: Basic ") + Base64Encode(ProxyAuth) + "\r\n";*/

   Req += "User-Agent: Debian APT-HTTP/1.2\r\n\r\n";
//   cout << Req << endl;
   
   Out.Read(Req);
}
									/*}}}*/
// HttpMethod::Go - Run a single loop					/*{{{*/
// ---------------------------------------------------------------------
/* This runs the select loop over the server FDs, Output file FDs and
   stdin. */
bool HttpMethod::Go(bool ToFile,ServerState *Srv)
{
   // Server has closed the connection
   if (Srv->ServerFd == -1 && Srv->In.WriteSpace() == false)
      return false;
   
   fd_set rfds,wfds,efds;
   FD_ZERO(&rfds);
   FD_ZERO(&wfds);
   FD_ZERO(&efds);
   
   // Add the server
   if (Srv->Out.WriteSpace() == true && Srv->ServerFd != -1) 
      FD_SET(Srv->ServerFd,&wfds);
   if (Srv->In.ReadSpace() == true && Srv->ServerFd != -1) 
      FD_SET(Srv->ServerFd,&rfds);
   
   // Add the file
   int FileFD = -1;
   if (File != 0)
      FileFD = File->Fd();
   
   if (Srv->In.WriteSpace() == true && ToFile == true && FileFD != -1)
      FD_SET(FileFD,&wfds);
   
   // Add stdin
   FD_SET(STDIN_FILENO,&rfds);
	  
   // Error Set
   if (FileFD != -1)
      FD_SET(FileFD,&efds);
   if (Srv->ServerFd != -1)
      FD_SET(Srv->ServerFd,&efds);

   // Figure out the max fd
   int MaxFd = FileFD;
   if (MaxFd < Srv->ServerFd)
      MaxFd = Srv->ServerFd;
   
   // Select
   struct timeval tv;
   tv.tv_sec = 120;
   tv.tv_usec = 0;
   int Res = 0;
   if ((Res = select(MaxFd+1,&rfds,&wfds,&efds,&tv)) < 0)
      return _error->Errno("select","Select failed");
   
   if (Res == 0)
   {
      _error->Error("Connection timed out");
      return ServerDie(Srv);
   }
   
   // Some kind of exception (error) on the sockets, die
   if ((FileFD != -1 && FD_ISSET(FileFD,&efds)) || 
       (Srv->ServerFd != -1 && FD_ISSET(Srv->ServerFd,&efds)))
      return _error->Error("Socket Exception");

   // Handle server IO
   if (Srv->ServerFd != -1 && FD_ISSET(Srv->ServerFd,&rfds))
   {
      errno = 0;
      if (Srv->In.Read(Srv->ServerFd) == false)
	 return ServerDie(Srv);
   }
	 
   if (Srv->ServerFd != -1 && FD_ISSET(Srv->ServerFd,&wfds))
   {
      errno = 0;
      if (Srv->Out.Write(Srv->ServerFd) == false)
	 return ServerDie(Srv);
   }

   // Send data to the file
   if (FileFD != -1 && FD_ISSET(FileFD,&wfds))
   {
      if (Srv->In.Write(FileFD) == false)
	 return _error->Errno("write","Error writing to output file");
   }

   // Handle commands from APT
   if (FD_ISSET(STDIN_FILENO,&rfds))
   {
      if (Run(true) != 0)
	 exit(100);
   }   
       
   return true;
}
									/*}}}*/
// HttpMethod::Flush - Dump the buffer into the file			/*{{{*/
// ---------------------------------------------------------------------
/* This takes the current input buffer from the Server FD and writes it
   into the file */
bool HttpMethod::Flush(ServerState *Srv)
{
   if (File != 0)
   {
      SetNonBlock(File->Fd(),false);
      if (Srv->In.WriteSpace() == false)
	 return true;
      
      while (Srv->In.WriteSpace() == true)
      {
	 if (Srv->In.Write(File->Fd()) == false)
	    return _error->Errno("write","Error writing to file");
	 if (Srv->In.IsLimit() == true)
	    return true;
      }

      if (Srv->In.IsLimit() == true || Srv->Encoding == ServerState::Closes)
	 return true;
   }
   return false;
}
									/*}}}*/
// HttpMethod::ServerDie - The server has closed the connection.	/*{{{*/
// ---------------------------------------------------------------------
/* */
bool HttpMethod::ServerDie(ServerState *Srv)
{
   // Dump the buffer to the file
   if (Srv->State == ServerState::Data)
   {
      SetNonBlock(File->Fd(),false);
      while (Srv->In.WriteSpace() == true)
      {
	 if (Srv->In.Write(File->Fd()) == false)
	    return _error->Errno("write","Error writing to the file");

	 // Done
	 if (Srv->In.IsLimit() == true)
	    return true;
      }
   }
   
   // See if this is because the server finished the data stream
   if (Srv->In.IsLimit() == false && Srv->State != ServerState::Header && 
       Srv->Encoding != ServerState::Closes)
   {
      if (errno == 0)
	 return _error->Error("Error reading from server Remote end closed connection");
      return _error->Errno("read","Error reading from server");
   }
   else
   {
      Srv->In.Limit(-1);

      // Nothing left in the buffer
      if (Srv->In.WriteSpace() == false)
	 return false;
      
      // We may have got multiple responses back in one packet..
      Srv->Close();
      return true;
   }
   
   return false;
}
									/*}}}*/
// HttpMethod::DealWithHeaders - Handle the retrieved header data	/*{{{*/
// ---------------------------------------------------------------------
/* We look at the header data we got back from the server and decide what
   to do. Returns 
     0 - File is open,
     1 - IMS hit
     3 - Unrecoverable error 
     4 - Error with error content page */
int HttpMethod::DealWithHeaders(FetchResult &Res,ServerState *Srv)
{
   // Not Modified
   if (Srv->Result == 304)
   {
      unlink(Queue->DestFile.c_str());
      Res.IMSHit = true;
      Res.LastModified = Queue->LastModified;
      return 1;
   }
   
   /* We have a reply we dont handle. This should indicate a perm server
      failure */
   if (Srv->Result < 200 || Srv->Result >= 300)
   {
      _error->Error("%u %s",Srv->Result,Srv->Code);
      if (Srv->HaveContent == true)
	 return 4;
      return 3;
   }

   // This is some sort of 2xx 'data follows' reply
   Res.LastModified = Srv->Date;
   Res.Size = Srv->Size;
   
   // Open the file
   delete File;
   File = new FileFd(Queue->DestFile,FileFd::WriteAny);
   if (_error->PendingError() == true)
      return 3;

   FailFile = Queue->DestFile;
   FailFd = File->Fd();
   FailTime = Srv->Date;
      
   // Set the expected size
   if (Srv->StartPos >= 0)
   {
      Res.ResumePoint = Srv->StartPos;
      ftruncate(File->Fd(),Srv->StartPos);
   }
      
   // Set the start point
   lseek(File->Fd(),0,SEEK_END);

   delete Srv->In.MD5;
   Srv->In.MD5 = new MD5Summation;
   
   // Fill the MD5 Hash if the file is non-empty (resume)
   if (Srv->StartPos > 0)
   {
      lseek(File->Fd(),0,SEEK_SET);
      if (Srv->In.MD5->AddFD(File->Fd(),Srv->StartPos) == false)
      {
	 _error->Errno("read","Problem hashing file");
	 return 3;
      }
      lseek(File->Fd(),0,SEEK_END);
   }
   
   SetNonBlock(File->Fd(),true);
   return 0;
}
									/*}}}*/
// HttpMethod::SigTerm - Handle a fatal signal				/*{{{*/
// ---------------------------------------------------------------------
/* This closes and timestamps the open file. This is neccessary to get 
   resume behavoir on user abort */
void HttpMethod::SigTerm(int)
{
   if (FailFd == -1)
      exit(100);
   close(FailFd);
   
   // Timestamp
   struct utimbuf UBuf;
   time(&UBuf.actime);
   UBuf.actime = FailTime;
   UBuf.modtime = FailTime;
   utime(FailFile.c_str(),&UBuf);
   
   exit(100);
}
									/*}}}*/
// HttpMethod::Loop - Main loop						/*{{{*/
// ---------------------------------------------------------------------
/* */
int HttpMethod::Loop()
{
   signal(SIGTERM,SigTerm);
   signal(SIGINT,SigTerm);
   
   ServerState *Server = 0;
   
   int FailCounter = 0;
   while (1)
   {
      if (FailCounter >= 2)
      {
	 Fail("Massive Server Brain Damage");
	 FailCounter = 0;
      }
      
      // We have no commands, wait for some to arrive
      if (Queue == 0)
      {
	 if (WaitFd(STDIN_FILENO) == false)
	    return 0;
      }
      
      // Run messages
      if (Run(true) != 0)
	 return 100;

      if (Queue == 0)
	 continue;
      
      // Connect to the server
      if (Server == 0 || Server->Comp(Queue->Uri) == false)
      {
	 delete Server;
	 Server = new ServerState(Queue->Uri,this);
      }
            
      // Connnect to the host
      if (Server->Open() == false)
      {
	 Fail();
	 continue;
      }
      
      // Queue the request
      SendReq(Queue,Server->Out);

      // Fetch the next URL header data from the server.
      switch (Server->RunHeaders())
      {
	 case 0:
	 break;
	 
	 // The header data is bad
	 case 2:
	 {
	    _error->Error("Bad header Data");
	    Fail();
	    continue;
	 }
	 
	 // The server closed a connection during the header get..
	 default:
	 case 1:
	 {
	    FailCounter++;
	    _error->DumpErrors();
	    Server->Close();
	    continue;
	 }
      };
      
      // Decide what to do.
      FetchResult Res;
      Res.Filename = Queue->DestFile;
      switch (DealWithHeaders(Res,Server))
      {
	 // Ok, the file is Open
	 case 0:
	 {
	    URIStart(Res);

	    // Run the data
	    bool Result =  Server->RunData();

	    // Close the file, destroy the FD object and timestamp it
	    FailFd = -1;
	    delete File;
	    File = 0;
	    
	    // Timestamp
	    struct utimbuf UBuf;
	    time(&UBuf.actime);
	    UBuf.actime = Server->Date;
	    UBuf.modtime = Server->Date;
	    utime(Queue->DestFile.c_str(),&UBuf);

	    // Send status to APT
	    if (Result == true)
	    {
	       Res.MD5Sum = Server->In.MD5->Result();
	       URIDone(Res);
	    }
	    else
	       Fail();

	    break;
	 }
	 
	 // IMS hit
	 case 1:
	 {
	    URIDone(Res);
	    break;
	 }
	 
	 // Hard server error, not found or something
	 case 3:
	 {
	    Fail();
	    break;
	 }

	 // We need to flush the data, the header is like a 404 w/ error text
	 case 4:
	 {
	    Fail();
	    
	    // Send to content to dev/null
	    File = new FileFd("/dev/null",FileFd::WriteExists);
	    Server->RunData();
	    delete File;
	    File = 0;
	    break;
	 }
	 
	 default:
	 Fail("Internal error");
	 break;
      }
      
      FailCounter = 0;
   }
   
   return 0;
}
									/*}}}*/

int main()
{
   HttpMethod Mth;
   
   return Mth.Loop();
}
