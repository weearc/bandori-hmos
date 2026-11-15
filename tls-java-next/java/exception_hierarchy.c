#include "exception_hierarchy.h"
#include <stdio.h>
#include <string.h>

/* Platform superclasses, child first. Applied only to classes the APK does not
 * define and whose superclass is still the synthesized default (Object), so a
 * chain declared by another runtime module is never overridden. */
static const char *const pairs[][2] = {
 /* The base chain must exist before throwable_reference_call() accepts any
  * exception method: it requires derives(receiver, Throwable).  Without these
  * edges a synthesized RuntimeException kept Object as its super and
  * Throwable.getMessage()/RuntimeException(Throwable) fell through as
  * MISSING_API (72bq's Gson and OkHttp RouteException paths). */
 {"java/lang/Throwable","java/lang/Object"},
 {"java/lang/Exception","java/lang/Throwable"},
 {"java/lang/RuntimeException","java/lang/Exception"},
 {"java/lang/Error","java/lang/Throwable"},
 {"java/lang/IllegalArgumentException","java/lang/RuntimeException"},
 {"java/lang/IllegalStateException","java/lang/RuntimeException"},
 {"java/lang/NullPointerException","java/lang/RuntimeException"},
 {"java/lang/UnsupportedOperationException","java/lang/RuntimeException"},
 {"java/lang/ClassCastException","java/lang/RuntimeException"},
 {"java/lang/NumberFormatException","java/lang/IllegalArgumentException"},
 {"java/lang/ArrayStoreException","java/lang/RuntimeException"},
 {"java/lang/NegativeArraySizeException","java/lang/RuntimeException"},
 {"java/lang/ArithmeticException","java/lang/RuntimeException"},
 {"java/lang/IndexOutOfBoundsException","java/lang/RuntimeException"},
 {"java/lang/ClassNotFoundException","java/lang/Exception"},
 {"java/lang/ReflectiveOperationException","java/lang/Exception"},
 {"java/lang/NoSuchFieldException","java/lang/ReflectiveOperationException"},
 {"java/lang/InstantiationException","java/lang/ReflectiveOperationException"},
 {"java/lang/IllegalAccessException","java/lang/ReflectiveOperationException"},
 {"java/lang/StackOverflowError","java/lang/VirtualMachineError"},
 {"java/lang/InternalError","java/lang/VirtualMachineError"},
 {"java/lang/UnsatisfiedLinkError","java/lang/LinkageError"},
 {"java/lang/AbstractMethodError","java/lang/IncompatibleClassChangeError"},
 {"java/lang/IllegalAccessError","java/lang/IncompatibleClassChangeError"},
 {"java/lang/InstantiationError","java/lang/IncompatibleClassChangeError"},
 {"java/lang/VerifyError","java/lang/LinkageError"},
 {"java/lang/ExceptionInInitializerError","java/lang/LinkageError"},
 {"java/lang/AssertionError","java/lang/Error"},
 {"java/lang/LinkageError","java/lang/Error"},
 {"java/lang/IncompatibleClassChangeError","java/lang/LinkageError"},
 {"java/lang/NoSuchMethodError","java/lang/IncompatibleClassChangeError"},
 {"java/lang/NoClassDefFoundError","java/lang/LinkageError"},
 {"java/lang/VirtualMachineError","java/lang/Error"},
 {"java/lang/OutOfMemoryError","java/lang/VirtualMachineError"},
 {"java/lang/InterruptedException","java/lang/Exception"},
 {"java/lang/CloneNotSupportedException","java/lang/Exception"},
 {"java/lang/ClassCastException","java/lang/RuntimeException"},
 {"java/lang/SecurityException","java/lang/RuntimeException"},
 {"java/lang/ArithmeticException","java/lang/RuntimeException"},
 {"java/lang/IndexOutOfBoundsException","java/lang/RuntimeException"},
 {"java/lang/ArrayIndexOutOfBoundsException","java/lang/IndexOutOfBoundsException"},
 {"java/lang/StringIndexOutOfBoundsException","java/lang/IndexOutOfBoundsException"},
 {"java/io/IOException","java/lang/Exception"},
 {"java/io/InterruptedIOException","java/io/IOException"},
 {"java/io/EOFException","java/io/IOException"},
 {"java/io/FileNotFoundException","java/io/IOException"},
 {"java/io/UnsupportedEncodingException","java/io/IOException"},
 {"java/io/UncheckedIOException","java/lang/RuntimeException"},
 {"java/net/SocketException","java/io/IOException"},
 {"java/net/ConnectException","java/net/SocketException"},
 {"java/net/NoRouteToHostException","java/net/SocketException"},
 {"java/net/PortUnreachableException","java/net/SocketException"},
 {"java/net/BindException","java/net/SocketException"},
 {"java/net/SocketTimeoutException","java/io/InterruptedIOException"},
 {"java/net/UnknownHostException","java/io/IOException"},
 {"java/net/ProtocolException","java/io/IOException"},
 {"java/net/UnknownServiceException","java/io/IOException"},
 {"java/net/MalformedURLException","java/io/IOException"},
 {"javax/net/ssl/SSLException","java/io/IOException"},
 {"javax/net/ssl/SSLHandshakeException","javax/net/ssl/SSLException"},
 {"javax/net/ssl/SSLPeerUnverifiedException","javax/net/ssl/SSLException"},
 {"javax/net/ssl/SSLProtocolException","javax/net/ssl/SSLException"},
 {"javax/net/ssl/SSLKeyException","javax/net/ssl/SSLException"},
 {"java/security/GeneralSecurityException","java/lang/Exception"},
 {"java/security/KeyException","java/security/GeneralSecurityException"},
 {"java/security/KeyManagementException","java/security/KeyException"},
 {"java/security/InvalidKeyException","java/security/KeyException"},
 {"java/security/KeyStoreException","java/security/GeneralSecurityException"},
 {"java/security/NoSuchAlgorithmException","java/security/GeneralSecurityException"},
 {"java/security/NoSuchProviderException","java/security/GeneralSecurityException"},
 {"java/security/SignatureException","java/security/GeneralSecurityException"},
 {"java/security/InvalidAlgorithmParameterException","java/security/GeneralSecurityException"},
 {"java/security/UnrecoverableEntryException","java/security/GeneralSecurityException"},
 {"java/security/UnrecoverableKeyException","java/security/UnrecoverableEntryException"},
 {"java/security/cert/CertificateException","java/security/GeneralSecurityException"},
 {"java/security/cert/CertificateEncodingException","java/security/cert/CertificateException"},
 {"java/security/cert/CertificateParsingException","java/security/cert/CertificateException"},
 {"java/security/cert/CertificateExpiredException","java/security/cert/CertificateException"},
 {"java/security/cert/CertificateNotYetValidException","java/security/cert/CertificateException"},
 {"java/security/cert/CertPathValidatorException","java/security/GeneralSecurityException"},
};

void exception_hierarchy_vm_start(struct dvm *v){
 struct dvm_class *object=dvm_find_class_or_external(v,"java/lang/Object");
 for(size_t i=0;i<sizeof pairs/sizeof pairs[0];i++){
  struct dvm_class *c=dvm_find_class_or_external(v,pairs[i][0]),*parent=dvm_find_class_or_external(v,pairs[i][1]);
  if(c&&parent&&c!=parent&&c->external&&(!c->super||c->super==object))c->super=parent;
 }
}

bool exception_hierarchy_class_exists(const char *desc){
 if(!desc||desc[0]!='L')return false;
 size_t n=strlen(desc);if(n<3||desc[n-1]!=';')return false;
 for(size_t i=0;i<sizeof pairs/sizeof pairs[0];i++)for(unsigned j=0;j<2;j++)
  if(strlen(pairs[i][j])==n-2&&!memcmp(desc+1,pairs[i][j],n-2))return true;
 return false;
}

static bool assignable(struct dvm *v,const char *from,const char *to){
 return dvm__class_assignable(v,dvm_find_class_or_external(v,from),dvm_find_class_or_external(v,to));
}
bool exception_hierarchy_selftest(struct dvm *v){
 bool ok=assignable(v,"java/net/SocketTimeoutException","java/io/IOException")&&
  assignable(v,"java/net/ConnectException","java/lang/Exception")&&
  assignable(v,"javax/net/ssl/SSLHandshakeException","java/io/IOException")&&
  assignable(v,"java/security/cert/CertificateExpiredException","java/security/GeneralSecurityException")&&
  assignable(v,"java/security/KeyManagementException","java/lang/Exception")&&
  !assignable(v,"java/io/IOException","java/lang/RuntimeException")&&
  !assignable(v,"javax/net/ssl/SSLException","java/security/GeneralSecurityException")&&
  !assignable(v,"java/lang/AssertionError","java/lang/Exception");
 if(ok){extern int sdk_trace_fprintf(FILE*,const char*,...);sdk_trace_fprintf(stderr,"EXCEPTION_HIERARCHY_SELFTEST PASS io_net_ssl_security=1 negative=1\n");}
 return ok;
}
