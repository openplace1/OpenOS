Import("env")

env.AppendUnique(LINKFLAGS=["-flto"])
