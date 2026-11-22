# Signatures of the game's imports (recomp/bridgegen.py reads this; see its docstring).
#   name   ret  args  [C|raw]  [=crt]
# Imports with no line here are stubs that stop the game with the name when reached.

# process (imp_libc.c)
exit                    v  i      C
abort                   v  -      C
__assert_rtn            v  iiii
getenv                  i  i
system                  i  i
sleep                   i  i
usleep                  i  i
alarm                   i  i
getpid                  i  -
getuid                  i  -      =h_getuid
geteuid                 i  -      =h_getuid
getgid                  i  -      =h_getuid
getegid                 i  -      =h_getuid
getpwuid                i  i
gethostname             i  ii
signal                  i  ii
sigaction               i  iii
__error                 i  -      C
sched_yield             i  -      =h_sched_yield
sched_get_priority_min  i  i      =h_zero1
sched_get_priority_max  i  i      =h_zero1

# memory
malloc                  i  i      =malloc
calloc                  i  ii     =calloc
realloc                 i  ii     =realloc
free                    v  i      =free
memchr                  i  iii    =memchr
memcmp                  i  iii    =memcmp
memcpy                  i  iii    =memcpy
memmove                 i  iii    =memmove
memset                  i  iii    =memset
__memcpy_chk            i  iiii
__memmove_chk           i  iiii
__memset_chk            i  iiii
memset_pattern16        v  iii
bzero                   v  ii

# strings
strlen                  i  i      =strlen
strcmp                  i  ii     =strcmp
strncmp                 i  iii    =strncmp
strcpy                  i  ii     =strcpy
strncpy                 i  iii    =strncpy
strcat                  i  ii     =strcat
strchr                  i  ii     =strchr
strrchr                 i  ii     =strrchr
strstr                  i  ii     =strstr
strpbrk                 i  ii     =strpbrk
strcspn                 i  ii     =strcspn
strtok                  i  ii     =strtok
strtol                  i  iii    =strtol
strtoul                 i  iii    =strtoul
strtoll                 l  iii    =strtoll
strtod                  d  ii     =strtod
atoi                    i  i      =atoi
atol                    i  i      =atol
atof                    d  i      =atof
__strcpy_chk            i  iii
__strcat_chk            i  iii
strlcpy                 i  iii
strlcat                 i  iii
strcasecmp              i  ii
strncasecmp             i  iii
strdup                  i  i
strtok_r                i  iii
strerror                i  i
strerror_r              i  iii
basename                i  i
__maskrune              i  ii
__tolower               i  i
__toupper               i  i

# random, sorting
rand                    i  -
srand                   v  i
random                  i  -
srandom                 v  i
srandomdev              v  -
arc4random              i  -
qsort                   v  iiii   C

# stdio
printf                  i  i+
vprintf                 i  ii
fprintf                 i  ii+
vfprintf                i  iii
sprintf                 i  ii+
snprintf                i  iii+
vsprintf                i  iii
vsnprintf               i  iiii
__sprintf_chk           i  iiii+
__snprintf_chk          i  iiiii+
__vsprintf_chk          i  iiiii
sscanf                  i  ii+
fscanf                  i  ii+
puts                    i  i
putchar                 i  i
perror                  v  i
fopen                   i  ii
fclose                  i  i      =h_fclose
fread                   i  iiii   =h_fread
fwrite                  i  iiii
fseek                   i  iii
ftell                   i  i      =h_ftell
rewind                  v  i      =h_rewind
feof                    i  i      =h_feof
ferror                  i  i      =h_ferror
fflush                  i  i      =h_fflush
fgets                   i  iii    =h_fgets
fputc                   i  ii
fputs                   i  ii
fileno                  i  i      =h_fileno
setvbuf                 i  iiii
remove                  i  i

# files
open                    i  ii+
close                   i  i
read                    i  iii
write                   i  iii
lseek                   l  ili
fcntl                   i  ii+
ioctl                   i  ii+
stat                    i  ii
fstat                   i  ii
statfs                  i  ii
getcwd                  i  ii
chdir                   i  i
utimes                  i  ii
setxattr                i  iiiiii

# time
time                    i  i
gettimeofday            i  ii
localtime               i  i
localtime_r             i  ii
gmtime_r                i  ii
mach_absolute_time      l  -
mach_timebase_info      i  i
mach_thread_self        i  -
CFAbsoluteTimeGetCurrent d -

# compiler helpers, atomics
__divsi3                i  ii
__modsi3                i  ii
__udivsi3               i  ii
__umodsi3               i  ii
__divdi3                l  ll
__udivdi3               l  ll
__umoddi3               l  ll
__fixdfdi               l  d
__fixunsdfdi            l  d
__floatdidf             d  l
__floatundidf           d  l
__floatundisf           f  l
OSAtomicAdd32           i  ii

# math
acos                    d  d      =acos
asin                    d  d      =asin
atan                    d  d      =atan
atan2                   d  dd     =atan2
cos                     d  d      =cos
sin                     d  d      =sin
tan                     d  d      =tan
exp                     d  d      =exp
log                     d  d      =log
log2                    d  d      =log2
pow                     d  dd     =pow
fmod                    d  dd     =fmod
ceil                    d  d      =ceil
floor                   d  d      =floor
round                   d  d      =round
trunc                   d  d      =trunc
fmax                    d  dd     =fmax
fmin                    d  dd     =fmin
ldexp                   d  di     =ldexp
acosf                   f  f      =acosf
atanf                   f  f      =atanf
atan2f                  f  ff     =atan2f
cosf                    f  f      =cosf
sinf                    f  f      =sinf
expf                    f  f      =expf
powf                    f  ff     =powf
ceilf                   f  f      =ceilf
floorf                  f  f      =floorf
roundf                  f  f      =roundf
lrintf                  i  f
modff                   f  fi

# Objective-C runtime (objc.c)
objc_msgSend            raw
objc_msgSend_stret      raw
objc_msgSendSuper2      raw
objc_msgSendSuper2_stret raw
objc_sync_enter         i  i
objc_sync_exit          i  i
class_getName           i  i
class_getSuperclass     i  i
object_getClass         i  i
sel_getName             i  i
sel_registerName        i  i
objc_lookUpClass        i  i
class_getInstanceMethod i  ii
class_getClassMethod    i  ii
method_getImplementation i i
method_setImplementation i ii
method_getName          i  i
method_getTypeEncoding  i  i
class_addMethod         i  iiii
class_copyMethodList    i  ii
class_copyPropertyList  i  ii
class_getProperty       i  ii
property_getName        i  i
property_getAttributes  i  i
objc_getProperty        i  iiii
objc_setProperty        v  iiiiii
objc_copyStruct         v  iiiii
objc_enumerationMutation v i
objc_getClassList       i  ii

# Foundation C functions (ns_string.c, ns_coll.c, ns_misc.c)
NSLog                   v  i+
NSLogv                  v  ii
NSSearchPathForDirectoriesInDomains i iii
NSHomeDirectory         i  -
NSClassFromString       i  i
NSStringFromClass       i  i
NSSelectorFromString    i  i
NSStringFromSelector    i  i
NSStringFromCGRect      i  iiii
NSStringFromCGSize      i  ii
NSSetUncaughtExceptionHandler v i
NSAllocateObject        i  iii

# CoreFoundation
CFRelease               v  i
CFAllocatorGetDefault   i  -
CFStringCreateWithCString i iii
CFStringGetCString      i  iiii
CFStringGetCStringPtr   i  ii
CFStringGetLength       i  i
CFStringGetMaximumSizeForEncoding i ii
CFStringCreateWithFormat i iii+
CFStringAppendFormat    v  iii+
CFStringAppend          v  ii
CFStringAppendCharacters v iii
CFStringCompare         i  iii
CFStringFind            v  iiii
CFStringHasPrefix       i  ii
CFStringHasSuffix       i  ii
CFStringCreateArrayBySeparatingStrings i iii
CFStringCreateCopy      i  ii
CFStringCreateMutable   i  ii
CFStringCreateMutableCopy i iii
CFStringCreateWithSubstring i iiii
CFArrayCreate           i  iiii
CFArrayCreateMutable    i  iii
CFArrayAppendValue      v  ii
CFArrayGetCount         i  i
CFArrayGetValueAtIndex  i  ii
CFArrayRemoveAllValues  v  i
CFDictionaryCreateMutable i iiii
CFDictionaryGetValue    i  ii
CFDictionaryGetValueIfPresent i iii
CFDictionarySetValue    v  iii
CFDictionaryRemoveValue v  ii
CFNumberGetValue        i  iii
CFRunLoopGetCurrent     i  -
CFRunLoopGetMain        i  -
CFRunLoopRunInMode      i  ili

# libstdc++: std::string (imp_cxx.c)
_ZNSsC1Ev               i  i
_ZNSsC1EPKcRKSaIcE      i  iii
_ZNSsC1EPKcmRKSaIcE     i  iiii
_ZNSsC1ERKSs            i  ii
_ZNSsD1Ev               v  i
_ZNSsaSERKSs            i  ii
_ZNSsaSEPKc             i  ii
_ZNSs6assignEPKc        i  ii
_ZNSs6assignEPKcm       i  iii
_ZNSs6assignERKSsmm     i  iiii
_ZNKSs5c_strEv          i  i
_ZNKSs4dataEv           i  i      =h_ZNKSs5c_strEv
_ZNKSs4sizeEv           i  i
_ZNKSs6lengthEv         i  i      =h_ZNKSs4sizeEv
_ZNKSs5emptyEv          i  i
_ZNKSsixEm              i  ii
_ZNSsixEm               i  ii
_ZNSs2atEm              i  ii
_ZNKSs5beginEv          i  i
_ZNKSs3endEv            i  i
_ZNSs5beginEv           i  i
_ZNSs3endEv             i  i
_ZNKSs9_M_ibeginEv      i  i
_ZNKSs7_M_iendEv        i  i
_ZNSs12_S_empty_repEv   i  -
_ZNSs12_Alloc_hiderC1EPcRKSaIcE i iii
_ZNSs13_S_copy_charsEPcPKcS1_ v iii
_ZNSs13_S_copy_charsEPcN9__gnu_cxx17__normal_iteratorIPKcSsEES4_ v iii
_ZNSs4_Rep10_M_destroyERKSaIcE v ii
_ZNSs4_Rep10_M_refdataEv i i
_ZNSs4_Rep26_M_set_length_and_sharableEm v ii
_ZNSs4_Rep9_S_createEmmRKSaIcE i iii
_ZNSs5clearEv           v  i
_ZNSs7reserveEm         v  ii
_ZNSs6appendEPKc        i  ii
_ZNSs6appendEPKcm       i  iii
_ZNSs6appendERKSs       i  ii
_ZNSs6appendERKSsmm     i  iiii
_ZNSs6appendEmc         i  iii
_ZNSspLEPKc             i  ii
_ZNSspLERKSs            i  ii
_ZNSspLEc               i  ii
_ZNSs9push_backEc       v  ii
_ZNSs6insertEmPKc       i  iii
_ZNSs6resizeEm          v  ii
_ZNSs5eraseEmm          i  iii
_ZNSs5eraseEN9__gnu_cxx17__normal_iteratorIPcSsEES2_ i iii
_ZNSs7replaceEmmPKc     i  iiii
_ZNSs7replaceEmmRKSs    i  iiii
_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_PKcS4_ i iiiii
_ZNKSs4findEPKcm        i  iii
_ZNKSs4findERKSsm       i  iii
_ZNKSs4findEcm          i  iii
_ZNKSs5rfindEPKcm       i  iii
_ZNKSs13find_first_ofEPKcm i iii
_ZNKSs7compareERKSs     i  ii
_ZNKSs7compareEPKc      i  ii
_ZNKSs6substrEmm        v  iiii
_ZNSaIcEC1Ev            v  i      =h_alloc_noop
_ZNSaIcED1Ev            v  i      =h_alloc_noop
_ZNSaIcED2Ev            v  i      =h_alloc_noop
_ZNSaIcEC1ERKS_         v  i      =h_alloc_noop
_ZNSaIcEC2ERKS_         v  i      =h_alloc_noop

# libstdc++: containers, ABI support
_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base  i i
_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base i i =h_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base
_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base  i i
_ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base i i =h_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base
_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_ v iiii
_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_ i ii
_ZNSt15_List_node_base4hookEPS_ v ii
_ZNSt15_List_node_base6unhookEv v i
_ZNSt15_List_node_base8transferEPS_S0_ v iii
_Znwm                   i  i
_Znam                   i  i      =h_Znwm
_ZdlPv                  v  i
_ZdaPv                  v  i      =h_ZdlPv
__cxa_atexit            i  iii
__cxa_guard_acquire     i  i
__cxa_guard_release     v  i
__cxa_guard_abort       v  i
__cxa_pure_virtual      v  -      C
_ZSt9terminatev         v  -      C
_ZNSt8ios_base4InitC1Ev v  i
_ZNSt8ios_base4InitD1Ev v  i      =h_ZNSt8ios_base4InitC1Ev

# pthreads (imp_pthread.c)
pthread_mutex_init      i  ii
pthread_mutex_destroy   i  i
pthread_mutex_lock      i  i
pthread_mutex_trylock   i  i
pthread_mutex_unlock    i  i
pthread_mutexattr_init  i  i
pthread_mutexattr_destroy i i
pthread_mutexattr_settype i ii
pthread_cond_init       i  ii
pthread_cond_destroy    i  i
pthread_cond_signal     i  i
pthread_cond_timedwait  i  iii
pthread_rwlock_init     i  ii
pthread_rwlock_destroy  i  i
pthread_rwlock_wrlock   i  i
pthread_rwlock_trywrlock i i
pthread_rwlock_tryrdlock i i
pthread_rwlock_unlock   i  i
pthread_attr_init       i  i
pthread_attr_destroy    i  i
pthread_attr_setstacksize i ii
pthread_create          i  iiii
pthread_self            i  -
pthread_join            i  ii
pthread_exit            v  i
pthread_cancel          i  i
pthread_testcancel      v  -
pthread_setcanceltype   i  ii
pthread_kill            i  ii
pthread_getschedparam   i  iii
pthread_setschedparam   i  iii

# exceptions (imp_unwind.c)
_Unwind_SjLj_Register   v  i      C
_Unwind_SjLj_Unregister v  i      C
_Unwind_SjLj_Resume     v  i      C
__cxa_allocate_exception i i
__cxa_free_exception    v  i
__cxa_throw             v  iii    C
__cxa_begin_catch       i  i      C
__cxa_end_catch         v  -      C
__cxa_rethrow           v  -      C
__cxa_call_unexpected   v  i      C
objc_exception_throw    v  i      C
objc_begin_catch        i  i      C
objc_end_catch          v  -      C
objc_exception_rethrow  v  -      C
_ZSt17__throw_bad_allocv v -      C
_ZSt19__throw_logic_errorPKc v i  C
_ZSt20__throw_length_errorPKc v i C
_ZSt20__throw_out_of_rangePKc v i C
_ZNSt13runtime_errorC1ERKSs i ii
_ZNSt13runtime_errorC2ERKSs i ii  =h_ZNSt13runtime_errorC1ERKSs
_ZNSt13runtime_errorD2Ev v i
_ZNSt13runtime_errorD1Ev v i      =h_ZNSt13runtime_errorD2Ev
_ZNKSt13runtime_error4whatEv i i

# longjmp (rt_core.c; setjmp call sites are compiled as GUEST_SETJMP)
longjmp                 raw
siglongjmp              raw

# Blocks, dispatch (blocks.c)
_Block_object_assign    v  iii
_Block_object_dispose   v  ii
dispatch_async          v  ii

# iostreams (imp_stream.c)
_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode i iii
_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev i i
_ZNSt14basic_ifstreamIcSt11char_traitsIcEE5closeEv v i
_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1Ev i i
_ZNSt13basic_fstreamIcSt11char_traitsIcEED1Ev i i
_ZNSt13basic_fstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode v iii
_ZNSt13basic_fstreamIcSt11char_traitsIcEE5closeEv v i
_ZNSt13basic_fstreamIcSt11char_traitsIcEE7is_openEv i i
_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode i ii
_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev i i
_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC1ERKSsSt13_Ios_Openmode i iii
_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev i i
_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode i ii
_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev i i
_ZNKSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv v ii
_ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv v ii
_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strERKSs v ii
_ZNSolsEi               i  ii
_ZNSolsEj               i  ii
_ZNSolsEm               i  ii
_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc i ii
_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c i ii
_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E i ii
_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St5_Setw i ii
_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E i ii
_ZNSolsEPFRSt8ios_baseS0_E i ii
_ZNSirsERi              i  ii
_ZNSirsERf              i  ii
_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E i ii
_ZNSi4readEPci          i  iii
_ZNSi5seekgExSt12_Ios_Seekdir i ili
_ZNSi5tellgEv           v  ii
_ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate v ii

# UIKit, the device (uikit.c)
UIApplicationMain       i  iiii
sysctlbyname            i  iiiii
sysctl                  i  iiiiii
uname                   i  i

# CoreGraphics, UIGraphics (cg.c; structs returned by value come through a first pointer argument)
CGRectGetMinX           f  R
CGRectGetMinY           f  R
CGRectGetMaxX           f  R
CGRectGetWidth          f  R
CGRectGetHeight         f  R
CGRectIsNull            i  R
CGRectIsEmpty           i  R
CGRectEqualToRect       i  RR
CGRectContainsPoint     i  RP
CGRectInset             v  iRff
CGRectOffset            v  iRff
CGRectIntersection      v  iRR
CGAffineTransformMakeRotation v if
CGAffineTransformMakeScale v iff
CGAffineTransformMakeTranslation v iff
CGAffineTransformConcat v  iTT
CGAffineTransformRotate v  iTf
CGAffineTransformScale  v  iTff
CGAffineTransformTranslate v iTff
CGColorSpaceCreateDeviceRGB i -
CGColorSpaceCreateDeviceGray i -
CGColorSpaceGetModel    i  i
CGColorSpaceRelease     v  i
CGColorGetComponents    i  i
CGColorGetNumberOfComponents i i
CGColorGetColorSpace    i  i
CGImageCreate           i  iiiiiiiiiii
CGImageCreateWithMask   i  ii
CGImageGetWidth         i  i
CGImageGetHeight        i  i
CGImageRelease          v  i
CGDataProviderCreateWithData i iiii
CGDataProviderRelease   v  i
CGGradientCreateWithColorComponents i iiii
CGGradientRelease       v  i
CGBitmapContextCreate   i  iiiiiii
CGBitmapContextCreateImage i i
CGContextRelease        v  i
CGContextSaveGState     v  i
CGContextRestoreGState  v  i
CGContextTranslateCTM   v  iff
CGContextScaleCTM       v  iff
CGContextSetLineWidth   v  if
CGContextSetRGBFillColor v iffff
CGContextSetRGBStrokeColor v iffff
CGContextSetFillColor   v  ii
CGContextSetStrokeColor v  ii
CGContextSetStrokeColorSpace v ii
CGContextBeginPath      v  i
CGContextClosePath      v  i
CGContextMoveToPoint    v  iff
CGContextAddLineToPoint v  iff
CGContextAddRect        v  iR
CGContextAddEllipseInRect v iR
CGContextAddArc         v  ifffffi
CGContextAddArcToPoint  v  ifffff
CGContextClip           v  i
CGContextClipToRect     v  iR
CGContextFillRect       v  iR
CGContextFillPath       v  i
CGContextFillEllipseInRect v iR
CGContextStrokePath     v  i
CGContextStrokeLineSegments v iii
CGContextDrawImage      v  iRi
CGContextDrawLinearGradient v iiPPi
UIGraphicsBeginImageContext v S
UIGraphicsGetCurrentContext i -
UIGraphicsGetImageFromCurrentImageContext i -
UIImageWriteToSavedPhotosAlbum v iiii
UIGraphicsEndImageContext v -

# CoreFoundation, the rest (cf.c)
CFBundleGetMainBundle   i  -
CFBundleGetIdentifier   i  i
CFBundleGetInfoDictionary i i
CFBundleGetValueForInfoDictionaryKey i ii
CFBundleCopyBundleURL   i  i
CFBundleGetVersionNumber i i
CFURLCopyFileSystemPath i  ii
CFURLCreateWithFileSystemPath i iiii
CFURLCreateStringByAddingPercentEscapes i iiiii
CFURLCreateStringByReplacingPercentEscapesUsingEncoding i iiii
CFUUIDCreate            i  i
CFUUIDCreateString      i  ii
CFReadStreamCreateWithFile i ii
CFReadStreamOpen        i  i
CFReadStreamRead        i  iii
CFReadStreamClose       v  i
CFBitVectorCreate       i  iii
CFBitVectorCreateMutableCopy i iii
CFBitVectorGetBitAtIndex i ii
CFBitVectorSetBitAtIndex v iii
CFNotificationCenterGetLocalCenter i -
CFNotificationCenterAddObserver v iiiiii

# dyld (dyld.c)
dladdr                  i  ii
dlsym                   i  ii
_dyld_image_count       i  -
_dyld_get_image_name    i  i
_dyld_get_image_header  i  i

# Security: keychain, certificates (sec.c)
SecItemAdd              i  ii
SecItemCopyMatching     i  ii
SecItemUpdate           i  ii
SecItemDelete           i  i
SecCertificateCreateWithData i ii
SecPolicyCreateBasicX509 i -
SecTrustCreateWithCertificates i iii
SecTrustEvaluate        i  ii
SecTrustCopyPublicKey   i  i
SecKeyGetBlockSize      i  i
SecKeyEncrypt           i  iiiiii
SecKeyDecrypt           i  iiiiii

# CommonCrypto (crypto.c)
CC_MD5                  i  iii
CC_SHA1                 i  iii
CC_SHA1_Init            i  i
CC_SHA1_Update          i  iii
CC_SHA1_Final           i  ii
CC_SHA256               i  iii
CCHmac                  v  iiiiii
CCCrypt                 raw

# the network: none (net.c)
socket                  i  iii
connect                 i  iii
bind                    i  iii
listen                  i  ii
accept                  i  iii
send                    i  iiii
recv                    i  iiii
sendto                  i  iiiiii
recvfrom                i  iiiiii
select                  i  iiiii
setsockopt              i  iiiii
getsockopt              i  iiiii
shutdown                i  ii
getpeername             i  iii
getsockname             i  iii
getaddrinfo             i  iiii
freeaddrinfo            v  i
gethostbyname           i  i
getifaddrs              i  i
freeifaddrs             v  i
if_nametoindex          i  i
inet_addr               i  i
inet_aton               i  ii
inet_ntoa               i  i
inet_ntop               i  iiii
inet_pton               i  iii
SCNetworkReachabilityCreateWithName i ii
SCNetworkReachabilityCreateWithAddress i ii
SCNetworkReachabilityGetFlags i ii
SCNetworkReachabilitySetCallback i iii
SCNetworkReachabilityScheduleWithRunLoop i iii
SCNetworkReachabilityUnscheduleFromRunLoop i iii

# OpenGL ES (gles.c; straight ones from gl_list.h)
glActiveTexture         v  i
glAlphaFunc             v  if
glAttachShader          v  ii
glBindBuffer            v  ii
glBindFramebuffer       v  ii
glBindFramebufferOES    v  ii     =h_glBindFramebuffer
glBindRenderbuffer      v  ii
glBindRenderbufferOES   v  ii     =h_glBindRenderbuffer
glBindTexture           v  ii
glBlendFunc             v  ii
glBlendFuncSeparateOES  v  iiii   =h_glBlendFuncSeparate
glBufferData            v  iiii
glCheckFramebufferStatus i i
glCheckFramebufferStatusOES i i   =h_glCheckFramebufferStatus
glClear                 v  i
glClearColor            v  ffff
glClearDepthf           v  f
glClearStencil          v  i
glClientActiveTexture   v  i
glColor4f               v  ffff
glColor4ub              v  iiii
glColorMask             v  iiii
glColorPointer          v  iiii
glCompileShader         v  i
glCompressedTexImage2D  v  iiiiiiii
glCreateProgram         i  -
glCreateShader          i  i
glCullFace              v  i
glCurrentPaletteMatrixOES v i
glDeleteBuffers         v  ii
glDeleteProgram         v  i
glDeleteShader          v  i
glDeleteTextures        v  ii
glDepthFunc             v  i
glDepthMask             v  i
glDisable               v  i
glDisableClientState    v  i
glDisableVertexAttribArray v i
glDrawArrays            v  iii
glDrawElements          v  iiii
glEnable                v  i
glEnableClientState     v  i
glEnableVertexAttribArray v i
glFlush                 v  -
glFogf                  v  if
glFogfv                 v  ii
glFramebufferRenderbuffer v iiii
glFramebufferRenderbufferOES v iiii =h_glFramebufferRenderbuffer
glFramebufferTexture2D  v  iiiii
glFramebufferTexture2DOES v iiiii =h_glFramebufferTexture2D
glFrontFace             v  i
glGenBuffers            v  ii
glGenFramebuffers       v  ii
glGenFramebuffersOES    v  ii     =h_glGenFramebuffers
glGenRenderbuffers      v  ii
glGenRenderbuffersOES   v  ii     =h_glGenRenderbuffers
glGenTextures           v  ii
glGenerateMipmap        v  i
glGetActiveAttrib       v  iiiiiii
glGetActiveUniform      v  iiiiiii
glGetAttribLocation     i  ii
glGetError              i  -
glGetFloatv             v  ii
glGetFramebufferAttachmentParameteriv v iiii
glGetFramebufferAttachmentParameterivOES v iiii =h_glGetFramebufferAttachmentParameteriv
glGetIntegerv           v  ii
glGetProgramInfoLog     v  iiii
glGetProgramiv          v  iii
glGetShaderInfoLog      v  iiii
glGetShaderiv           v  iii
glGetString             i  i
glGetUniformLocation    i  ii
glHint                  v  ii
glLightModelfv          v  ii
glLightf                v  iif
glLightfv               v  iii
glLineWidth             v  f
glLinkProgram           v  i
glLoadIdentity          v  -
glLoadMatrixf           v  i
glMapBufferOES          i  ii
glMaterialf             v  iif
glMaterialfv            v  iii
glMatrixIndexPointerOES v  iiii
glMatrixMode            v  i
glMultMatrixf           v  i
glNormalPointer         v  iii
glPixelStorei           v  ii
glPointSize             v  f
glPopMatrix             v  -
glPushMatrix            v  -
glReadPixels            v  iiiiiii
glRenderbufferStorage   v  iiii
glRenderbufferStorageOES v iiii   =h_glRenderbufferStorage
glRotatef               v  ffff
glScissor               v  iiii
glShaderSource          v  iiii
glStencilFunc           v  iii
glStencilMask           v  i
glStencilOp             v  iii
glTexCoordPointer       v  iiii
glTexEnvf               v  iif
glTexEnvfv              v  iii
glTexEnvi               v  iii
glTexImage2D            v  iiiiiiiii
glTexParameterf         v  iif
glTexParameteri         v  iii
glTexSubImage2D         v  iiiiiiiii
glUniform1f             v  if
glUniform1fv            v  iii
glUniform1i             v  ii
glUniform1iv            v  iii
glUniform2f             v  iff
glUniform2fv            v  iii
glUniform2i             v  iii
glUniform2iv            v  iii
glUniform3f             v  ifff
glUniform3fv            v  iii
glUniform3i             v  iiii
glUniform3iv            v  iii
glUniform4f             v  iffff
glUniform4fv            v  iii
glUniform4i             v  iiiii
glUniform4iv            v  iii
glUniformMatrix2fv      v  iiii
glUniformMatrix3fv      v  iiii
glUniformMatrix4fv      v  iiii
glUnmapBufferOES        i  i
glUseProgram            v  i
glVertexAttribPointer   v  iiiiii
glVertexPointer         v  iiii
glViewport              v  iiii
glWeightPointerOES      v  iiii

# OpenAL, AudioSession (al.c)
alcOpenDevice           i  i
alcCloseDevice          i  i
alcCreateContext        i  ii
alcDestroyContext       v  i
alcMakeContextCurrent   i  i
alcGetCurrentContext    i  -
alcGetContextsDevice    i  i
alcProcessContext       v  i
alcSuspendContext       v  i
alcGetProcAddress       i  ii
alGenBuffers            v  ii
alDeleteBuffers         v  ii
alBufferData            v  iiiii
alGenSources            v  ii
alDeleteSources         v  ii
alSourcePlay            v  i
alSourcePause           v  i
alSourceStop            v  i
alSourceQueueBuffers    v  iii
alSourceUnqueueBuffers  v  iii
alSourcei               v  iii
alSourcef               v  iif
alSource3f              v  iifff
alSourcefv              v  iii
alGetSourcei            v  iii
alListener3f            v  ifff
alListenerfv            v  ii
alDistanceModel         v  i
AudioSessionInitialize  i  iiii
AudioSessionSetActive   i  i
AudioSessionSetProperty i  iii
AudioSessionGetProperty i  iii
AudioSessionAddPropertyListener i iii
AudioSessionRemovePropertyListenerWithUserData i iii
AudioServicesPlaySystemSound v i
