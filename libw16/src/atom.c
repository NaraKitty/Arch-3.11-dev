/* Atoms: KERNEL's local atom table (AddAtom, DeleteAtom, FindAtom, GetAtomName) and USER's global
 * one (GlobalAddAtom ...). As in 3.1 a string atom is 0xC000 or above and counts its AddAtom calls;
 * names compare without case and keep the spelling of the first AddAtom (at most 255 characters).
 * "#nnn" and MAKEINTATOM(n) with 0 < n < 0xC000 are integer atoms: n itself, never stored. */
#include "w16int.h"

typedef struct {
    char *name;
    int refs;
} AtomEnt;
typedef struct {
    AtomEnt *e;
    int n;
} AtomTable;

static AtomTable local_atoms, global_atoms;

#define MAXINTATOM 0xC000

/* integer atom of a MAKEINTATOM value or a "#nnn" string; 0 if the name is a string atom */
static ATOM int_atom(LPCSTR name, int *is_int)
{
    *is_int = 1;
    if (IS_INTRESOURCE(name)) return (ATOM)(uintptr_t)name < MAXINTATOM ? (ATOM)(uintptr_t)name : 0;
    if (name[0] != '#') { *is_int = 0; return 0; }
    unsigned v = 0;
    for (const char *p = name + 1; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (unsigned)(*p - '0');
        if (v >= MAXINTATOM) return 0;
    }
    return (ATOM)v;
}

static int find(AtomTable *t, LPCSTR name)
{
    for (int i = 0; i < t->n; i++)
        if (t->e[i].refs && !lstrcmpi(t->e[i].name, name)) return i;
    return -1;
}

static ATOM add(AtomTable *t, LPCSTR name)
{
    int is_int;
    ATOM a = int_atom(name, &is_int);
    if (is_int) return a;
    if (!*name || strlen(name) > 255) return 0;
    int i = find(t, name);
    if (i >= 0) {
        t->e[i].refs++;
        return (ATOM)(MAXINTATOM + i);
    }
    for (i = 0; i < t->n && t->e[i].refs; i++) ;
    if (i == t->n) {
        if (t->n >= 0x10000 - MAXINTATOM) return 0;
        AtomEnt *e = realloc(t->e, sizeof *e * (t->n + 1));
        if (!e) return 0;
        t->e = e;
        t->n++;
    }
    t->e[i].name = strdup(name);
    if (!t->e[i].name) return 0;
    t->e[i].refs = 1;
    return (ATOM)(MAXINTATOM + i);
}

/* 0 when done, the atom when it is not in the table */
static ATOM del(AtomTable *t, ATOM a)
{
    if (a < MAXINTATOM) return 0;
    int i = a - MAXINTATOM;
    if (i >= t->n || !t->e[i].refs) return a;
    if (--t->e[i].refs == 0) {
        free(t->e[i].name);
        t->e[i].name = NULL;
    }
    return 0;
}

static ATOM look(AtomTable *t, LPCSTR name)
{
    int is_int;
    ATOM a = int_atom(name, &is_int);
    if (is_int) return a;
    int i = find(t, name);
    return i >= 0 ? (ATOM)(MAXINTATOM + i) : 0;
}

static UINT name_of(AtomTable *t, ATOM a, LPSTR buf, int cb)
{
    char num[8];
    const char *s;
    if (cb <= 0 || !a) return 0;
    if (a < MAXINTATOM) {
        wsprintf(num, "#%u", (unsigned)a);
        s = num;
    } else {
        int i = a - MAXINTATOM;
        if (i >= t->n || !t->e[i].refs) { buf[0] = 0; return 0; }
        s = t->e[i].name;
    }
    int n = (int)strlen(s);
    if (n > cb - 1) n = cb - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return (UINT)n;
}

ATOM AddAtom(LPCSTR name) { return add(&local_atoms, name); }
ATOM DeleteAtom(ATOM atom) { return del(&local_atoms, atom); }
ATOM FindAtom(LPCSTR name) { return look(&local_atoms, name); }
UINT GetAtomName(ATOM atom, LPSTR buf, int cb) { return name_of(&local_atoms, atom, buf, cb); }
ATOM GlobalAddAtom(LPCSTR name) { return add(&global_atoms, name); }
ATOM GlobalDeleteAtom(ATOM atom) { return del(&global_atoms, atom); }
ATOM GlobalFindAtom(LPCSTR name) { return look(&global_atoms, name); }
UINT GlobalGetAtomName(ATOM atom, LPSTR buf, int cb) { return name_of(&global_atoms, atom, buf, cb); }

BOOL InitAtomTable(int n) { (void)n; return TRUE; }

/* USER's own table: RegisterWindowMessage and RegisterClipboardFormat (USER seg1:8214) both AddAtom
 * with USER's data segment, so a message and a clipboard format of the same name get the same value;
 * GetClipboardFormatName reads it back (seg39:004B) and SetClipboardData/EmptyClipboard raise and drop
 * a registered format's count (KERNEL GetAtomHandle, DeleteAtom) */
static AtomTable user_atoms;
ATOM w16_user_atom_add(LPCSTR name) { return add(&user_atoms, name); }
ATOM w16_user_atom_delete(ATOM atom) { return del(&user_atoms, atom); }
UINT w16_user_atom_name(ATOM atom, LPSTR buf, int cb) { return name_of(&user_atoms, atom, buf, cb); }
void w16_user_atom_ref(ATOM atom)
{
    int i = atom - MAXINTATOM;
    if (atom >= MAXINTATOM && i < user_atoms.n && user_atoms.e[i].refs) user_atoms.e[i].refs++;
}
