#include <node.h>
#include <node_events.h>
#include <node_buffer.h>
#include <assert.h>
#include <pcap/pcap.h>
#include <v8.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ev.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

using namespace v8;
using namespace node;

// These things need to be moved into a class, so each instance can have its own session.
// Right now, there can only be one pcap session at a time.
struct bpf_program fp;
bpf_u_int32 mask;
bpf_u_int32 net;
pcap_t *pcap_handle;
Buffer *buffer;

// PacketReady is called from within pcap, still on the stack of Dispatch.  It should be called
// only one time per Dispatch, but sometimes it gets called 0 times.  PacketReady invokes the
// JS callback associated with the dispatch() call in JS.
//
// Stack:
// 1. readWatcher.callback (pcap.js)
// 2. binding.dispatch (pcap.js)
// 3. Dispatch (binding.cc)
// 4. pcap_dispatch (libpcap)
// 5. PacketReady (binding.cc)
// 6. binding.dispatch callback (pcap.js)
//
void PacketReady(u_char *callback_p, const struct pcap_pkthdr* pkthdr, const u_char* packet) {
    HandleScope scope;

    Local<Function> * callback = (Local<Function>*)callback_p;

    // TODO - bounds checking
    memcpy(buffer->data(), packet, pkthdr->caplen);

    TryCatch try_catch;

    Local<Object> packet_header = Object::New();

    packet_header->Set(String::New("tv_sec"), Integer::NewFromUnsigned(pkthdr->ts.tv_sec));
    packet_header->Set(String::New("tv_usec"), Integer::NewFromUnsigned(pkthdr->ts.tv_usec));
    packet_header->Set(String::New("caplen"), Integer::NewFromUnsigned(pkthdr->caplen));
    packet_header->Set(String::New("len"), Integer::NewFromUnsigned(pkthdr->len));

    Local<Value> argv[1] = { packet_header };

    (*callback)->Call(Context::GetCurrent()->Global(), 1, argv);

    if (try_catch.HasCaught())  {
        FatalException(try_catch);
    }
}

Handle<Value>
Dispatch(const Arguments& args)
{
    HandleScope scope;

    if (args.Length() != 2) {
        return ThrowException(Exception::TypeError(String::New("Dispatch takes exactly two arguments")));
    }

    if (!Buffer::HasInstance(args[0])) {
        return ThrowException(Exception::TypeError(String::New("First argument must be a buffer")));
    }

    if (!args[1]->IsFunction()) {
        return ThrowException(Exception::TypeError(String::New("Second argument must be a function")));
    }

    Local<Function> callback = Local<Function>::Cast(args[1]);

    buffer = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());

    int packet_count, total_packets = 0;
    do {
        packet_count = pcap_dispatch(pcap_handle, 1, PacketReady, (u_char *)&callback);
        total_packets += packet_count;
    } while (packet_count > 0);

    return scope.Close(Integer::NewFromUnsigned(total_packets));
}

Handle<Value>
Open(bool live, const Arguments& args)
{
    HandleScope scope;
    char errbuf[PCAP_ERRBUF_SIZE];

    if (args.Length() != 2 || !args[0]->IsString() || !args[1]->IsString()) {
        return ThrowException(Exception::TypeError(String::New("Bad arguments")));
    }
    String::Utf8Value device(args[0]->ToString());
    String::Utf8Value filter(args[1]->ToString());

    if (live) {
        if (pcap_lookupnet((char *) *device, &net, &mask, errbuf) == -1) {
            net = 0;
            mask = 0;
            fprintf(stderr, "warning: %s - filtering may not work right\n", errbuf);
        }

        pcap_handle = pcap_create((char *) *device, errbuf);
        if (pcap_handle == NULL) {
            return ThrowException(Exception::Error(String::New(errbuf)));
        }

        // 64KB is the max IPv4 packet size
        if (pcap_set_snaplen(pcap_handle, 65535) != 0) {
            return ThrowException(Exception::Error(String::New("error setting snaplen")));
        }

        // always use promiscuous mode
        if (pcap_set_promisc(pcap_handle, 1) != 0) {
            return ThrowException(Exception::Error(String::New("error setting promiscuous mode")));
        }

        // Try to set a 10MB buffer size.  Sometimes the OS has a lower limit that it will silently enforce.
        // TODO - make this settable at runtime
        if (pcap_set_buffer_size(pcap_handle, 10 * 1024 * 1024) != 0) {
            return ThrowException(Exception::Error(String::New("error setting buffer size")));
        }

        // set "timeout" on read, even though we are also setting nonblock below.  On Linux this is required.
        if (pcap_set_timeout(pcap_handle, 1000) != 0) {
            return ThrowException(Exception::Error(String::New("error setting read timeout")));
        }

        // TODO - pass in an option to enable rfmon on supported interfaces.  Sadly, it can be a disruptive
        // operation, so we can't just always try to turn it on.
        // if (pcap_set_rfmon(pcap_handle, 1) != 0) {
        //     return ThrowException(Exception::Error(String::New(pcap_geterr(pcap_handle))));
        // }

        if (pcap_activate(pcap_handle) != 0) {
            return ThrowException(Exception::Error(String::New(pcap_geterr(pcap_handle))));
        }
    } else {
        // Device is the path to the savefile
        pcap_handle = pcap_open_offline((char *) *device, errbuf);
        if (pcap_handle == NULL) {
            return ThrowException(Exception::Error(String::New(errbuf)));
        }
    }

    if (pcap_setnonblock(pcap_handle, 1, errbuf) == -1) {
        return ThrowException(Exception::Error(String::New(errbuf)));
    }

    // TODO - if filter is empty, don't bother with compile or set
    if (pcap_compile(pcap_handle, &fp, (char *) *filter, 1, net) == -1) {
        return ThrowException(Exception::Error(String::New(pcap_geterr(pcap_handle))));
    }

    if (pcap_setfilter(pcap_handle, &fp) == -1) {
        return ThrowException(Exception::Error(String::New(pcap_geterr(pcap_handle))));
    }

    // Work around buffering bug in BPF on OSX 10.6 as of May 19, 2010
    // This may result in dropped packets under load because it disables the (broken) buffer
    // http://seclists.org/tcpdump/2010/q1/110
#if defined(__APPLE_CC__) || defined(__APPLE__)
    #include <net/bpf.h>
    int fd = pcap_get_selectable_fd(pcap_handle);
    int v = 1;
    ioctl(fd, BIOCIMMEDIATE, &v);
    // TODO - check return value
#endif

    int link_type = pcap_datalink(pcap_handle);

    Local<Value> ret;
    switch (link_type) {
    case DLT_NULL:
        ret = String::New("LINKTYPE_NULL");
        break;
    case DLT_EN10MB: // most wifi interfaces pretend to be "ethernet"
        ret =  String::New("LINKTYPE_ETHERNET");
        break;
    case DLT_IEEE802_11_RADIO: // 802.11 "monitor mode"
        ret = String::New("LINKTYPE_IEEE802_11_RADIO");
        break;
    case DLT_RAW: // "raw IP"
        ret = String::New("LINKTYPE_RAW");
        break;
    default:
        snprintf(errbuf, PCAP_ERRBUF_SIZE, "Unknown linktype %d", link_type);
        ret = String::New(errbuf);
        break;
    }
    return scope.Close(ret);
}

Handle<Value>
OpenLive(const Arguments& args)
{
    return Open(true, args);
}

Handle<Value>
OpenOffline(const Arguments& args)
{
    return Open(false, args);
}

Handle<Value>
FindAllDevs(const Arguments& args)
{
    HandleScope scope;
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_if_t *alldevsp, *cur_dev;
    
    if (pcap_findalldevs(&alldevsp, errbuf) == -1) {
        return ThrowException(Exception::TypeError(String::New(errbuf)));
    }

    Local<Array> DevsArray = Array::New();

    int i = 0;
    for (cur_dev = alldevsp ; cur_dev != NULL ; cur_dev = cur_dev->next, i++) {
        Local<Object> Dev = Object::New();

        Dev->Set(String::New("name"), String::New(cur_dev->name));
        if (cur_dev->description != NULL) {
            Dev->Set(String::New("description"), String::New(cur_dev->description));
        }
        Local<Array> AddrArray = Array::New();
        int j = 0;
        for (pcap_addr_t *cur_addr = cur_dev->addresses ; cur_addr != NULL ; cur_addr = cur_addr->next, j++) {
            if (cur_addr->addr && cur_addr->addr->sa_family == AF_INET) {
                Local<Object> Address = Object::New();
                
                struct sockaddr_in *sin = (struct sockaddr_in *) cur_addr->addr;
                Address->Set(String::New("addr"), String::New(inet_ntoa(sin->sin_addr)));
                
                if (cur_addr->netmask != NULL) {
                    sin = (struct sockaddr_in *) cur_addr->netmask;
                    Address->Set(String::New("netmask"), String::New(inet_ntoa(sin->sin_addr)));
                }
                if (cur_addr->broadaddr != NULL) {
                    sin = (struct sockaddr_in *) cur_addr->broadaddr;
                    Address->Set(String::New("broadaddr"), String::New(inet_ntoa(sin->sin_addr)));
                }
                if (cur_addr->dstaddr != NULL) {
                    sin = (struct sockaddr_in *) cur_addr->dstaddr;
                    Address->Set(String::New("dstaddr"), String::New(inet_ntoa(sin->sin_addr)));
                }
                AddrArray->Set(Integer::New(j), Address);
            }
            // TODO - support AF_INET6
        }
        
        Dev->Set(String::New("addresses"), AddrArray);

        if (cur_dev->flags & PCAP_IF_LOOPBACK) {
            Dev->Set(String::New("flags"), String::New("PCAP_IF_LOOPBACK"));
        }

        DevsArray->Set(Integer::New(i), Dev);
    }

    pcap_freealldevs(alldevsp);
    return scope.Close(DevsArray);
}

Handle<Value>
Close(const Arguments& args)
{
    HandleScope scope;

    pcap_close(pcap_handle);

    return Undefined();
}

Handle<Value>
Fileno(const Arguments& args)
{
    HandleScope scope;

    int fd = pcap_get_selectable_fd(pcap_handle);

    return scope.Close(Integer::NewFromUnsigned(fd));
}

Handle<Value>
Stats(const Arguments& args)
{
    HandleScope scope;

    struct pcap_stat ps;

    if (pcap_stats(pcap_handle, &ps) == -1) {
        return ThrowException(Exception::Error(String::New("Error in pcap_stats")));
        // TODO - use pcap_geterr to figure out what the error was
    }

    Local<Object> stats_obj = Object::New();

    stats_obj->Set(String::New("ps_recv"), Integer::NewFromUnsigned(ps.ps_recv));
    stats_obj->Set(String::New("ps_drop"), Integer::NewFromUnsigned(ps.ps_drop));
    stats_obj->Set(String::New("ps_ifdrop"), Integer::NewFromUnsigned(ps.ps_ifdrop));
    // ps_ifdrop may not be supported on this platform, but there's no good way to tell
    
    return scope.Close(stats_obj);
}

Handle<Value>
DefaultDevice(const Arguments& args)
{
    HandleScope scope;
    char errbuf[PCAP_ERRBUF_SIZE];
    
    // Look up the first device with an address, pcap_lookupdev() just returns
    // the first non-loopback device.
    Local<Value> ret;
    pcap_if_t *alldevs, *dev;
    pcap_addr_t *addr;
    bool found = false;

    if (pcap_findalldevs(&alldevs, errbuf) == -1) {
        return ThrowException(Exception::Error(String::New(errbuf)));
    }

    for (dev = alldevs; dev != NULL; dev = dev->next) {
        if (dev->addresses != NULL && !(dev->flags & PCAP_IF_LOOPBACK)) {
            for (addr = dev->addresses; addr != NULL; addr = addr->next) {
                if (addr->addr->sa_family == AF_INET || addr->addr->sa_family == AF_INET6) {
                    ret = String::New(dev->name);
                    found = true;
                    break;
                }
            }

            if (found) {
                break;
            }
        }
    }

    pcap_freealldevs(alldevs);
    return scope.Close(ret);
}

Handle<Value>
LibVersion(const Arguments &args)
{
    HandleScope scope;

    return scope.Close(String::New(pcap_lib_version()));
}

extern "C" void init (Handle<Object> target)
{
    HandleScope scope;

    target->Set(String::New("findalldevs"), FunctionTemplate::New(FindAllDevs)->GetFunction());
    target->Set(String::New("open_live"), FunctionTemplate::New(OpenLive)->GetFunction());
    target->Set(String::New("open_offline"), FunctionTemplate::New(OpenOffline)->GetFunction());
    target->Set(String::New("dispatch"), FunctionTemplate::New(Dispatch)->GetFunction());
    target->Set(String::New("fileno"), FunctionTemplate::New(Fileno)->GetFunction());
    target->Set(String::New("close"), FunctionTemplate::New(Close)->GetFunction());
    target->Set(String::New("stats"), FunctionTemplate::New(Stats)->GetFunction());
    target->Set(String::New("default_device"), FunctionTemplate::New(DefaultDevice)->GetFunction());
    target->Set(String::New("lib_version"), FunctionTemplate::New(LibVersion)->GetFunction());
}
