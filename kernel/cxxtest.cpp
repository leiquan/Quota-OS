/* Guest check that the freestanding C++ runtime can construct an object. */
struct CxxProbe {
    int value;
    CxxProbe() : value(41) {}
    virtual int bump() { return value + 1; }
    virtual ~CxxProbe() {}
};

extern "C" int cxx_runtime_test(void)
{
    CxxProbe *p = new CxxProbe;
    if (!p)
        return -1;
    int n = p->bump();
    delete p;
    return n;
}
