"""Run the production input uevent selection with a host/guest/broadcast fixture."""
from pathlib import Path
import subprocess
import tempfile
k = Path(__file__).resolve().parents[4]
s = (k / 'drivers/input/input.c').read_text()
a = s.index('static int input_dev_uevent(')
body = s[a:s.index('\n}\n', a)+3]
h = (k / 'drivers/input/andr36oid-guest.h').read_text()
map_functions = h[h.index('static inline unsigned int a36_guest_key'):h.index('\n#endif')]
source = r'''
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define KEY_CNT (KEY_MAX+1)
#define WORDS ((KEY_CNT+8*sizeof(long)-1)/(8*sizeof(long)))
#define DECLARE_BITMAP(n,c) unsigned long n[WORDS]
#define bitmap_zero(p,n) memset(p,0,WORDS*sizeof(long))
#define test_bit(k,p) (((p)[(k)/(8*sizeof(long))]>>((k)%(8*sizeof(long))))&1)
#define __set_bit(k,p) ((p)[(k)/(8*sizeof(long))]|=1UL<<((k)%(8*sizeof(long))))
#define for_each_set_bit(k,p,n) for(k=0;k<(n);k++) if(test_bit(k,p))
struct input_dev {struct input_id id; const char *name,*phys,*uniq; unsigned long keybit[WORDS],evbit[WORDS],propbit[WORDS],relbit[WORDS],absbit[WORDS],mscbit[WORDS],ledbit[WORDS],sndbit[WORDS],ffbit[WORDS],swbit[WORDS];};
struct device {struct input_dev *input;};
struct kobj_uevent_env {bool sysfs_read;};
#define to_input_dev(d) ((d)->input)
static bool guest_mode;
static bool a36_guest_pad(struct input_dev *dev) {(void)dev;return guest_mode;}
static unsigned long seen[WORDS],alias[WORDS];
#define INPUT_ADD_HOTPLUG_VAR(...) do {} while(0)
#define INPUT_ADD_HOTPLUG_BM_VAR(name,bits,max) do {if(!strcmp(name,"KEY="))memcpy(seen,bits,sizeof(seen));} while(0)
#define INPUT_ADD_HOTPLUG_MODALIAS_VAR(dev,bits) memcpy(alias,bits,sizeof(alias))
'''+map_functions+body+r'''
int main(void) {
    struct input_dev input={0};struct device dev={.input=&input};
    __set_bit(EV_KEY,input.evbit);__set_bit(KEY_UP,input.keybit);__set_bit(KEY_HOMEPAGE,input.keybit);
    unsigned long original[WORDS];memcpy(original,input.keybit,sizeof(original));
    for(int mode=0;mode<4;mode++) {
        guest_mode=mode&1;struct kobj_uevent_env env={.sysfs_read=!!(mode&2)};
        assert(!input_dev_uevent(&dev,&env));
        bool translated=guest_mode && env.sysfs_read;
        assert(test_bit(BTN_DPAD_UP,seen)==translated);
        assert(test_bit(BTN_TRIGGER_HAPPY5,seen)==translated);
        assert(test_bit(KEY_UP,seen)==!translated);
        assert(test_bit(KEY_HOMEPAGE,seen)==!translated);
        assert(!memcmp(alias,seen,sizeof(seen)));
        assert(!memcmp(original,input.keybit,sizeof(original)));
    }
    puts("PASS: sysfs uevent KEY and MODALIAS follow guest read ABI; host reads and broadcasts in either caller context retain host bits");
}
'''
with tempfile.TemporaryDirectory(prefix='a36-uevent-') as temp:
    p = Path(temp) / 'test.c'
    p.write_text(source)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
