// General descriptor-relative filesystem, process and digest primitives.
// Application storage layouts and policies do not belong here.
#include "runtime/sputnik_ext.h"
#include "package/distribution.h"
#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct FD {
  int n;
  explicit FD(int value):n(value) { if(n<0) throw std::runtime_error(std::strerror(errno)); }
  ~FD(){if(n>=0)::close(n);}
  FD(const FD&)=delete;
};
std::string text(SputnikCtx*c,SputnikValue v) {
  const char*p;size_t n;
  if(!sputnik_str_view(c,v,&p,&n))throw std::runtime_error("expected Str");
  std::string s(p,n);
  if(s.find('\0')!=std::string::npos)throw std::runtime_error("NUL in text");
  return s;
}
int64_t number(SputnikCtx*c,SputnikValue v) {
  int64_t n;if(!sputnik_as_int(c,v,&n))throw std::runtime_error("expected Int");return n;
}
bool boolean(SputnikCtx*c,SputnikValue v) {
  int n;if(!sputnik_as_bool(c,v,&n))throw std::runtime_error("expected Bool");return n;
}
mode_t permissions(SputnikCtx*c,SputnikValue v) {
  auto n=number(c,v);if(n<0||n>07777)throw std::runtime_error("invalid permissions");return n;
}
SputnikValue string(SputnikCtx*c,const std::string&s){return sputnik_make_str(c,s.data(),s.size());}
SputnikValue list(SputnikCtx*c,const std::vector<SputnikValue>&s){return sputnik_make_list(c,s.data(),s.size());}
SputnikStatus fault(SputnikCtx*c,const std::exception&e){return sputnik_fault(c,"IOError",e.what());}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<std::string> parts(const std::string&path) {
  require(path.empty()||path[0]!='/',"member path must be relative");
  std::vector<std::string> out;size_t start=0;
  while(start<path.size()){
    auto end=path.find('/',start);auto part=path.substr(start,end-start);
    require(!part.empty()&&part!="."&&part!="..","unsafe member path");
    out.push_back(part);if(end==std::string::npos)break;start=end+1;
  }
  return out;
}
int walk(int fd,const std::vector<std::string>&path,size_t count,bool create=false,mode_t mode=0700) {
  FD current(dup(fd));
  for(size_t i=0;i<count;++i){
    if(create&&mkdirat(current.n,path[i].c_str(),mode)&&errno!=EEXIST)throw std::runtime_error("mkdir failed");
    FD next(openat(current.n,path[i].c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(create)require(fsync(current.n)==0,"directory fsync failed");
    ::close(current.n);current.n=next.n;next.n=-1;
  }
  auto out=current.n;current.n=-1;return out;
}
int handle(SputnikCtx*c,SputnikValue v,const char*tag) {
  void*p=nullptr;require(sputnik_handle_ptr(c,v,tag,&p)&&p,"closed resource");
  return static_cast<FD*>(p)->n;
}
SputnikValue owned(SputnikCtx*c,std::unique_ptr<FD>p,const char*tag) {
  auto out=sputnik_make_handle(c,tag,p.get());require(out,"cannot create handle");p.release();return out;
}
SputnikValue info(SputnikCtx*c,const struct stat&st) {
  return list(c,{sputnik_make_int(c,st.st_size),sputnik_make_int(c,st.st_mtime),
    sputnik_make_int(c,st.st_mode&07777),sputnik_make_int(c,st.st_uid),
    sputnik_make_bool(c,S_ISREG(st.st_mode)),sputnik_make_bool(c,S_ISDIR(st.st_mode)),
    sputnik_make_bool(c,S_ISLNK(st.st_mode))});
}
std::string hex(const unsigned char*p,size_t n){
  std::string out;const char*h="0123456789abcdef";
  for(size_t i=0;i<n;++i){out+=h[p[i]>>4];out+=h[p[i]&15];}return out;
}
struct Hash {
  EVP_MD_CTX*p=EVP_MD_CTX_new();bool finished=false;
  Hash(){require(p&&EVP_DigestInit_ex(p,EVP_sha256(),nullptr)==1,"digest init failed");}
  ~Hash(){if(p)EVP_MD_CTX_free(p);}
  void add(const void*data,size_t n){require(!finished&&EVP_DigestUpdate(p,data,n)==1,"digest update failed");}
  std::string finish(){unsigned char bytes[32];unsigned n=0;require(!finished&&EVP_DigestFinal_ex(p,bytes,&n)==1,"digest finalize failed");finished=true;return hex(bytes,n);}
};
Hash* hash(SputnikCtx*c,SputnikValue v){
  void*p=nullptr;require(sputnik_handle_ptr(c,v,"sdk.digest.SHA256",&p)&&p,"closed digest");return static_cast<Hash*>(p);
}
// The core has already verified this inventory. Export only the bounded build
// document with its verified member digest; never compile/load package bytes.
std::string inspected_build(int fd,const sputnik::pkg::DistributionInfo&facts) {
  const sputnik::pkg::DistributionMember* member=nullptr;
  for(const auto& item:facts.members)if(item.path=="meta/build.json")member=&item;
  if(!member)return "{\"bytecode\":{},\"native\":[]}";
  const auto offset=lseek(fd,0,SEEK_CUR);require(offset>=0,"cannot seek package");
  struct Restore {int fd;off_t offset;~Restore(){lseek(fd,offset,SEEK_SET);}} restore{fd,offset};
  require(lseek(fd,0,SEEK_SET)>=0,"cannot seek package");
  std::unique_ptr<archive,decltype(&archive_read_free)> reader(archive_read_new(),archive_read_free);
  require(bool(reader),"cannot allocate archive reader");
  archive_read_support_filter_gzip(reader.get());archive_read_support_format_tar(reader.get());
  require(archive_read_open_fd(reader.get(),fd,65536)==ARCHIVE_OK,"cannot read verified package");
  archive_entry* entry=nullptr;
  while(archive_read_next_header(reader.get(),&entry)==ARCHIVE_OK){
    const char* name=archive_entry_pathname(entry);
    if(!name||std::string(name)!="meta/build.json"){require(archive_read_data_skip(reader.get())==ARCHIVE_OK,"cannot skip package member");continue;}
    std::string text;std::array<char,65536>buffer;Hash hash;
    for(;;){auto n=archive_read_data(reader.get(),buffer.data(),buffer.size());require(n>=0,"cannot read build metadata");if(!n)break;
      require(text.size()+static_cast<size_t>(n)<=1048576,"build metadata exceeds limit");text.append(buffer.data(),n);hash.add(buffer.data(),n);}
    require(text.size()==member->size&&hash.finish()==member->sha256,"build metadata changed after inspection");
    return text;
  }
  throw std::runtime_error("verified build metadata disappeared");
}
volatile sig_atomic_t termination=0;
void stop(int){termination=1;}
}
#define LEAF(name) extern "C" SputnikStatus name(SputnikCtx*c,const SputnikValue*a,size_t,SputnikValue*out)
#define METHOD(name) extern "C" SputnikStatus name(SputnikCtx*c,SputnikValue self,const SputnikValue*a,size_t,SputnikValue*out)
#define TRY try {
#define DONE return SPUTNIK_OK; } catch(const std::exception&e){return fault(c,e);}
#define NIL *out=sputnik_make_null(c)

LEAF(spfs_absolute) { TRY *out=string(c,std::filesystem::absolute(text(c,a[0])).lexically_normal().string()); DONE }
LEAF(spfs_root_new) { TRY
  auto path=text(c,a[0]);require(!path.empty()&&path[0]=='/',"Root requires absolute path");
  FD base(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));auto p=parts(path.substr(1));
  auto fd=std::make_unique<FD>(walk(base.n,p,p.size(),boolean(c,a[1]),permissions(c,a[2])));
  *out=owned(c,std::move(fd),"sdk.fs.Root"); DONE
}
extern "C" void spfs_root_free(SputnikCtx*,void*p){delete static_cast<FD*>(p);}
extern "C" void spfs_file_free(SputnikCtx*,void*p){delete static_cast<FD*>(p);}
METHOD(spfs_root_open) { TRY
  auto p=parts(text(c,a[0]));require(!p.empty(),"empty file path");FD parent(walk(handle(c,self,"sdk.fs.Root"),p,p.size()-1));
  auto mode=text(c,a[1]);bool create=boolean(c,a[2]),exclusive=boolean(c,a[3]),truncate=boolean(c,a[4]);
  require(!exclusive||create,"exclusive requires create");
  int flags=O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK;
  if(mode=="read"){flags|=O_RDONLY;require(!truncate,"cannot truncate read-only file");}
  else if(mode=="write")flags|=O_WRONLY;
  else if(mode=="append"){flags|=O_WRONLY|O_APPEND;require(!truncate,"cannot truncate append file");}
  else if(mode=="read_write")flags|=O_RDWR;
  else throw std::runtime_error("unknown file mode");
  if(create)flags|=O_CREAT;if(exclusive)flags|=O_EXCL;if(truncate)flags|=O_TRUNC;
  auto fd=std::make_unique<FD>(openat(parent.n,p.back().c_str(),flags,permissions(c,a[5])));
  struct stat st;require(!fstat(fd->n,&st)&&S_ISREG(st.st_mode),"not a regular file");
  *out=owned(c,std::move(fd),"sdk.fs.File"); DONE
}
METHOD(spfs_root_mkdir) { TRY
  auto p=parts(text(c,a[0]));require(!p.empty(),"empty directory path");
  bool parents=boolean(c,a[1]);auto mode=permissions(c,a[2]);int root=handle(c,self,"sdk.fs.Root");
  if(parents){FD result(walk(root,p,p.size(),true,mode));}
  else {FD parent(walk(root,p,p.size()-1));require(!mkdirat(parent.n,p.back().c_str(),mode)||errno==EEXIST,"mkdir failed");FD result(openat(parent.n,p.back().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));require(!fsync(parent.n),"directory fsync failed");}
  NIL; DONE
}
METHOD(spfs_root_metadata) { TRY
  auto p=parts(text(c,a[0]));struct stat st;
  if(p.empty()){require(!fstat(handle(c,self,"sdk.fs.Root"),&st),"stat failed");*out=info(c,st);return SPUTNIK_OK;}
  FD parent(walk(handle(c,self,"sdk.fs.Root"),p,p.size()-1));
  if(fstatat(parent.n,p.back().c_str(),&st,AT_SYMLINK_NOFOLLOW)){if(errno==ENOENT){NIL;return SPUTNIK_OK;}throw std::runtime_error("stat failed");}
  *out=info(c,st); DONE
}
METHOD(spfs_root_entries) { TRY
  auto p=parts(text(c,a[0]));auto limit=number(c,a[1]);require(limit>0&&limit<=1000000,"invalid entry limit");
  FD dir(walk(handle(c,self,"sdk.fs.Root"),p,p.size()));DIR*raw=fdopendir(dup(dir.n));require(raw,"listing failed");
  std::unique_ptr<DIR,decltype(&closedir)> entries(raw,closedir);std::vector<SputnikValue> rows;
  errno=0;while(auto entry=readdir(entries.get())){std::string name=entry->d_name;if(name=="."||name=="..")continue;rows.push_back(string(c,name));if(rows.size()>=static_cast<size_t>(limit))break;errno=0;}
  require(!errno,"listing failed");*out=list(c,rows); DONE
}
METHOD(spfs_root_remove) { TRY
  auto p=parts(text(c,a[0]));require(!p.empty(),"empty path");FD parent(walk(handle(c,self,"sdk.fs.Root"),p,p.size()-1));
  require(!unlinkat(parent.n,p.back().c_str(),0)||(errno==ENOENT&&boolean(c,a[1])),"remove failed");require(!fsync(parent.n),"directory fsync failed");NIL; DONE
}
METHOD(spfs_root_rename) { TRY
  auto source=parts(text(c,a[0])),target=parts(text(c,a[1]));require(!source.empty()&&!target.empty(),"empty path");int root=handle(c,self,"sdk.fs.Root");
  FD from(walk(root,source,source.size()-1)),to(walk(root,target,target.size()-1));
  require(!renameat(from.n,source.back().c_str(),to.n,target.back().c_str()),"rename failed");
  require(!fsync(from.n)&&!fsync(to.n),"directory fsync failed");NIL; DONE
}
METHOD(spfs_root_link) { TRY
  auto source=parts(text(c,a[0])),target=parts(text(c,a[1]));require(!source.empty()&&!target.empty(),"empty path");int root=handle(c,self,"sdk.fs.Root");
  FD from(walk(root,source,source.size()-1)),to(walk(root,target,target.size()-1));struct stat st;
  require(!fstatat(from.n,source.back().c_str(),&st,AT_SYMLINK_NOFOLLOW)&&S_ISREG(st.st_mode),"link source not regular");
  if(linkat(from.n,source.back().c_str(),to.n,target.back().c_str(),0)){require(errno==EEXIST,"link failed");*out=sputnik_make_bool(c,0);return SPUTNIK_OK;}
  require(!fsync(to.n),"directory fsync failed");*out=sputnik_make_bool(c,1); DONE
}
METHOD(spfs_root_sync) { TRY
  auto p=parts(text(c,a[0]));FD directory(walk(handle(c,self,"sdk.fs.Root"),p,p.size()));require(!fsync(directory.n),"directory fsync failed");NIL; DONE
}
METHOD(spfs_root_chmod) { TRY
  auto p=parts(text(c,a[0]));require(!p.empty(),"empty path");FD parent(walk(handle(c,self,"sdk.fs.Root"),p,p.size()-1));
  FD f(openat(parent.n,p.back().c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));require(!fchmod(f.n,permissions(c,a[1]))&&!fsync(f.n),"chmod failed");NIL; DONE
}
METHOD(spfs_root_lock) { TRY
  auto p=parts(text(c,a[0]));require(!p.empty(),"empty lock path");FD parent(walk(handle(c,self,"sdk.fs.Root"),p,p.size()-1));
  auto fd=std::make_unique<FD>(openat(parent.n,p.back().c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600));struct stat st;
  require(!fstat(fd->n,&st)&&S_ISREG(st.st_mode),"lock not regular");
  require(!flock(fd->n,LOCK_EX|(boolean(c,a[1])?LOCK_NB:0)),"lock unavailable");*out=owned(c,std::move(fd),"sdk.fs.File"); DONE
}
METHOD(spfs_file_write) { TRY
  const uint8_t*p;size_t n;require(sputnik_bytes_view(c,a[0],&p,&n),"expected Bytes");size_t offset=0;int fd=handle(c,self,"sdk.fs.File");
  while(offset<n){auto count=write(fd,p+offset,n-offset);if(count<0&&errno==EINTR)continue;require(count>0,"write failed");offset+=count;}
  *out=sputnik_make_int(c,n); DONE
}
METHOD(spfs_file_read) { TRY
  auto count=number(c,a[0]);require(count>0&&count<=16777216,"read size must be 1..16777216");std::vector<uint8_t>b(count);ssize_t n;
  do{n=read(handle(c,self,"sdk.fs.File"),b.data(),b.size());}while(n<0&&errno==EINTR);
  require(n>=0,"read failed");*out=n?sputnik_make_bytes(c,b.data(),n):sputnik_make_null(c); DONE
}
METHOD(spfs_file_sync) { TRY require(!fsync(handle(c,self,"sdk.fs.File")),"file fsync failed");NIL; DONE }
METHOD(spfs_file_truncate) { TRY auto n=number(c,a[0]);require(n>=0&&!ftruncate(handle(c,self,"sdk.fs.File"),n),"truncate failed");NIL; DONE }
METHOD(spfs_file_metadata) { TRY struct stat st;require(!fstat(handle(c,self,"sdk.fs.File"),&st),"stat failed");*out=info(c,st); DONE }
METHOD(spfs_file_sha256) { TRY
  int fd=handle(c,self,"sdk.fs.File");Hash h;std::array<char,65536>b;off_t offset=0;ssize_t n;
  while((n=pread(fd,b.data(),b.size(),offset))!=0){if(n<0&&errno==EINTR)continue;require(n>0,"digest read failed");h.add(b.data(),n);offset+=n;}
  *out=string(c,h.finish()); DONE
}
LEAF(sphash_new) { TRY auto h=std::make_unique<Hash>();*out=sputnik_make_handle(c,"sdk.digest.SHA256",h.get());require(*out,"cannot create hash");h.release(); DONE }
extern "C" void sphash_free(SputnikCtx*,void*p){delete static_cast<Hash*>(p);}
METHOD(sphash_update) { TRY const uint8_t*p;size_t n;require(sputnik_bytes_view(c,a[0],&p,&n),"expected Bytes");hash(c,self)->add(p,n);NIL; DONE }
METHOD(sphash_finish) { TRY *out=string(c,hash(c,self)->finish()); DONE }

LEAF(spos_env) { TRY auto key=text(c,a[0]);const char*value=getenv(key.c_str());*out=string(c,value?value:""); DONE }
LEAF(spos_uid) { *out=sputnik_make_int(c,geteuid());return SPUTNIK_OK; }
LEAF(spos_umask) { TRY *out=sputnik_make_int(c,umask(permissions(c,a[0]))); DONE }
LEAF(spos_executable) { TRY
  std::array<char,4096>b{};
#ifdef __APPLE__
  uint32_t n=b.size();require(!_NSGetExecutablePath(b.data(),&n),"executable path too long");
#else
  auto n=readlink("/proc/self/exe",b.data(),b.size()-1);require(n>=0,"executable path unavailable");
#endif
  *out=string(c,std::filesystem::canonical(b.data()).string()); DONE
}
LEAF(spos_trap_termination) { TRY
  termination=0;struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
  require(!sigaction(SIGTERM,&action,nullptr)&&!sigaction(SIGINT,&action,nullptr),"signal handler failed");NIL; DONE
}
LEAF(spos_termination_requested) { *out=sputnik_make_bool(c,termination);return SPUTNIK_OK; }
LEAF(spos_limits) { TRY
  for(size_t i=0;i<3;++i){if(sputnik_is_null(c,a[i]))continue;auto n=number(c,a[i]);require(n>=0,"negative resource limit");
    int kind=i==0?RLIMIT_CORE:i==1?RLIMIT_NOFILE:RLIMIT_AS;
#ifdef __APPLE__
    if(i==2)continue;
#endif
    rlimit value{static_cast<rlim_t>(n),static_cast<rlim_t>(n)};require(!setrlimit(kind,&value),"resource limit failed");}
  NIL; DONE
}
LEAF(spos_stdin_text) { TRY
  auto limit=number(c,a[0]);require(limit>=0&&limit<=16777216,"invalid stdin limit");std::string result;char b[8192];ssize_t n;
  while((n=read(STDIN_FILENO,b,sizeof b))!=0){if(n<0&&errno==EINTR)continue;require(n>0,"stdin read failed");result.append(b,n);require(result.size()<=static_cast<size_t>(limit),"stdin limit exceeded");}
  *out=string(c,result); DONE
}
LEAF(sppkg_inspect) { TRY
  int fd=handle(c,a[0],"sdk.fs.File");auto facts=sputnik::pkg::inspect_distribution_fd(fd,text(c,a[1]));auto&m=facts.manifest;
  auto deps=[&](const std::vector<sputnik::pkg::PackageDependency>&items){std::vector<SputnikValue>out;for(auto&d:items)out.push_back(list(c,{string(c,d.name),string(c,d.version),string(c,d.source)}));return list(c,out);};
  std::vector<SputnikValue>modules,kinds;
  for(auto&mod:m.modules){std::string hash;for(auto&member:facts.members)if(member.path==mod.path)hash=member.sha256;modules.push_back(list(c,{string(c,mod.name),string(c,mod.representation),string(c,mod.path),string(c,hash)}));}
  for(auto&ext:m.native_extensions)kinds.push_back(string(c,ext.kind));
  for(auto&provider:m.native_providers)if(!provider.libraries.empty())kinds.push_back(string(c,"prebuilt"));
  auto strings=[&](const std::vector<std::string>&items){std::vector<SputnikValue>out;for(const auto&item:items)out.push_back(string(c,item));return list(c,out);};
  *out=list(c,{string(c,sputnik::pkg::distribution_to_json(facts)),string(c,sputnik::pkg::manifest_to_json(m)),
    string(c,sputnik::pkg::render_distribution_manifest(m)),string(c,m.kind),string(c,m.limit_profile),string(c,m.bundle),
    list(c,{string(c,m.target.os),string(c,m.target.arch),string(c,m.target.min_os),string(c,m.target.libc),string(c,m.target.min_libc),string(c,m.target.compute_backend),strings(m.target.cpu_features),strings(m.target.compute_backends),string(c,m.target.cuda_version),string(c,m.target.min_cuda_driver),strings(m.target.cuda_architectures)}),
    deps(m.artifact_dependencies),deps(m.artifact_build_dependencies),list(c,modules),list(c,kinds),string(c,inspected_build(fd,facts)),
    sputnik_make_bool(c,!m.native_extensions.empty()||!m.native_providers.empty()||!m.native_requirements.empty())}); DONE
}
