#include "cert_selftest.h"
#include "cert_objects.h"
#include "fixture_der.h"
#include "expected_spki.h"
#include <string.h>
int cert_objects_selftest(void){
 static const unsigned char sha256_abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
 static const unsigned char sha1_abc[20]={0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
 unsigned char digest[32];if(cert_sha256((const unsigned char*)"abc",3,digest)||memcmp(digest,sha256_abc,32))return 1;
 if(cert_sha1((const unsigned char*)"abc",3,digest)||memcmp(digest,sha1_abc,20))return 2;
 cert_object *cert=cert_object_parse(fixture_valid,sizeof fixture_valid);if(!cert)return 3;
 size_t size=0;const unsigned char *der=cert_object_der(cert,&size);
 if(!der||size!=sizeof fixture_valid||memcmp(der,fixture_valid,size)){cert_object_free(cert);return 4;}
 const unsigned char *spki=cert_object_spki(cert,&size);
 int failed=!spki||!size||cert_sha256(spki,size,digest)||memcmp(digest,expected_spki_hash,32);
 cert_object_free(cert);if(failed)return 5;
 cert=cert_object_parse((const unsigned char*)"invalid",7);if(cert){cert_object_free(cert);return 6;}return 0;
}
