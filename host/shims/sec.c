/* sec.c -- the Security framework the game imports.

   The keychain (SecItemAdd / CopyMatching / Update / Delete) is a list of item dictionaries kept in
   home/Library/Keychains/keychain.plist: an item matches a query when every attribute the query names
   (not its kSecReturn / kSecMatch / kSecValue keys) is equal.  Certificates, trust and RSA keys are only
   used by the online parts: they fail (errSecParam) or give nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "ns.h"

#define errSecSuccess 0
#define errSecParam ((u32)-50)
#define errSecDuplicateItem ((u32)-25299)
#define errSecItemNotFound ((u32)-25300)

static id items;                   /* NSMutableArray of NSMutableDictionary */
static char path[512];

static void load(void)
{
    if (items)
        return;
    snprintf(path, sizeof path, "%s/Library/Keychains", gt_guest_home);
    char host[1024];
    CreateDirectoryA(gt_host_path(path, host, sizeof host), NULL);
    snprintf(path, sizeof path, "%s/Library/Keychains/keychain.plist", gt_guest_home);
    items = plist_read_file(path);
    if (!items || !ns_is_array(items)) {
        objc_release(items);
        items = ns_array_new(NULL, 0);
    }
}
static void save(void) { plist_write_file(path, items); }

static int control_key(id k)
{
    const char *s = ns_utf8(k);
    return !strncmp(s, "SecReturn", 9) || !strncmp(s, "SecMatch", 8) || !strcmp(s, "SecValueData") ||
           !strcmp(s, "SecValueRef") || !strcmp(s, "SecValuePersistentRef");
}
static int matches(id item, id query)
{
    for (u32 i = 0, n = ns_dict_count(query); i < n; i++) {
        id k = ns_dict_key_at(query, i);
        if (control_key(k))
            continue;
        id a = ns_dict_get(item, k);
        if (!a || !ns_equal(a, ns_dict_val_at(query, i)))
            return 0;
    }
    return 1;
}
static int find(id query, u32 from)
{
    for (u32 i = from, n = ns_array_count(items); i < n; i++)
        if (matches(ns_array_at(items, i), query))
            return (int)i;
    return -1;
}
static int flag(id query, const char *key)
{
    id v = ns_dict_get(query, ns_string(key));
    return v && ns_is_number(v) && ns_num_int(v);
}

/* query and attributes are host dictionaries or game NSDictionary objects: copy into a host one */
static id host_dict(id d) { return d ? msg0(d, "mutableCopy") : ns_dict_new(); }

u32 h_SecItemAdd(u32 attrs, u32 result)
{
    load();
    id a = host_dict(attrs);
    if (find(a, 0) >= 0) {
        objc_release(a);
        return errSecDuplicateItem;
    }
    ns_array_add(items, a);
    objc_release(a);
    save();
    if (result)
        wr32(result, 0);
    return errSecSuccess;
}
u32 h_SecItemCopyMatching(u32 query0, u32 result)
{
    load();
    id query = objc_autorelease(host_dict(query0));
    if (result)
        wr32(result, 0);
    int i = find(query, 0);
    if (i < 0)
        return errSecItemNotFound;
    id item = ns_array_at(items, (u32)i);
    id out = 0;
    if (flag(query, "SecReturnAttributes")) {
        out = msg0(item, "mutableCopy");
        if (!flag(query, "SecReturnData"))
            msg1(out, "removeObjectForKey:", ns_string("SecValueData"));
    } else if (flag(query, "SecReturnData")) {
        out = objc_retain(ns_dict_get(item, ns_string("SecValueData")));
    }
    if (result)
        wr32(result, out);
    else
        objc_release(out);
    return errSecSuccess;
}
u32 h_SecItemUpdate(u32 query0, u32 attrs)
{
    load();
    id query = objc_autorelease(host_dict(query0));
    int i = find(query, 0), any = 0;
    for (; i >= 0; i = find(query, (u32)i + 1)) {
        id item = ns_array_at(items, (u32)i);
        msg1(item, "addEntriesFromDictionary:", attrs);
        any = 1;
    }
    if (!any)
        return errSecItemNotFound;
    save();
    return errSecSuccess;
}
u32 h_SecItemDelete(u32 query0)
{
    load();
    id query = objc_autorelease(host_dict(query0));
    int any = 0;
    for (int i; (i = find(query, 0)) >= 0;) {
        msg1(items, "removeObjectAtIndex:", (u32)i);
        any = 1;
    }
    if (!any)
        return errSecItemNotFound;
    save();
    return errSecSuccess;
}

u32 h_SecCertificateCreateWithData(u32 alloc, u32 data) { (void)alloc, (void)data; return 0; }
u32 h_SecPolicyCreateBasicX509(void) { return 0; }
u32 h_SecTrustCreateWithCertificates(u32 certs, u32 policies, u32 out)
{
    (void)certs, (void)policies;
    if (out)
        wr32(out, 0);
    return errSecParam;
}
u32 h_SecTrustEvaluate(u32 trust, u32 result)
{
    (void)trust;
    if (result)
        wr32(result, 0);
    return errSecParam;
}
u32 h_SecTrustCopyPublicKey(u32 trust) { (void)trust; return 0; }
u32 h_SecKeyGetBlockSize(u32 key) { (void)key; return 128; }
u32 h_SecKeyEncrypt(u32 k, u32 pad, u32 in, u32 inlen, u32 out, u32 outlen)
{
    (void)k, (void)pad, (void)in, (void)inlen, (void)out;
    if (outlen)
        wr32(outlen, 0);
    return errSecParam;
}
u32 h_SecKeyDecrypt(u32 k, u32 pad, u32 in, u32 inlen, u32 out, u32 outlen)
{
    return h_SecKeyEncrypt(k, pad, in, inlen, out, outlen);
}
