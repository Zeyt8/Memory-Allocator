# SPDX-License-Identifier: BSD-3-Clause

import os
import sys
import difflib
from subprocess import Popen, PIPE


verbose = False
TRACED_CALLS = ["os_malloc", "os_calloc", "os_realloc", "os_free", "brk", "mmap", "munmap"]
TESTS = {
    "test-malloc-no-preallocate": 2,
    "test-malloc-preallocate": 3,
    "test-malloc-arrays": 5,
    "test-malloc-block-reuse": 3,
    "test-malloc-expand-block": 2,
    "test-malloc-no-split": 2,
    "test-malloc-split-one-block": 3,
    "test-malloc-split-first": 2,
    "test-malloc-split-last": 2,
    "test-malloc-split-middle": 3,
    "test-malloc-split-vector": 2,
    "test-malloc-coalesce": 3,
    "test-malloc-coalesce-big": 3,
    "test-calloc-no-preallocate": 1,
    "test-calloc-preallocate": 1,
    "test-calloc-arrays": 5,
    "test-calloc-block-reuse": 1,
    "test-calloc-expand-block": 1,
    "test-calloc-no-split": 1,
    "test-calloc-split-one-block": 1,
    "test-calloc-split-first": 1,
    "test-calloc-split-last": 1,
    "test-calloc-split-middle": 1,
    "test-calloc-split-vector": 2,
    "test-calloc-coalesce": 2,
    "test-calloc-coalesce-big": 2,
    "test-realloc-no-preallocate": 1,
    "test-realloc-preallocate": 1,
    "test-realloc-arrays": 3,
    "test-realloc-block-reuse": 3,
    "test-realloc-expand-block": 2,
    "test-realloc-no-split": 3,
    "test-realloc-split-one-block": 3,
    "test-realloc-split-first": 3,
    "test-realloc-split-last": 3,
    "test-realloc-split-middle": 2,
    "test-realloc-split-vector": 2,
    "test-realloc-coalesce": 3,
    "test-realloc-coalesce-big": 1,
    "test-all": 5,
}


class Call:
    replacePairs = {
        "SYS_": "",
        "@SYS": "",
        "@libosmem.so": "",
        "_checked": "",
        "<unfinished ...>": ") = <void>",
        "nil": "0"
    }

    def __init__(self, line: str) -> None:
        for s, r in Call.replacePairs.items():
            line = line.replace(s, r)

        self.name = line.split("(")[0]
        self.args = line[line.find("(")+1:line.find(")")].replace(" ", "").split(",")
        self.ret = line[line.find("=")+1:].strip()

    def __repr__(self) -> str:
        return f"{self.name} ({self.args}) = {self.ret}"


class SysCall(Call):
    # mmap prot values
    MMAP_PROTS = {
        "PROT_READ": 0x1,
        "PROT_WRITE": 0x2,
        "PROT_EXEC": 0x4,
        "PROT_NONE": 0x0
    }

    # mmap flags values
    MMAP_FLAGS = {
        "MAP_SHARED": 0x01,
        "MAP_PRIVATE": 0x02,
        "MAP_FIXED": 0x10,
        "MAP_ANON": 0x20,
    }

    def __init__(self, line: str) -> None:
        Call.__init__(self, line)
        if self.name == "brk":
            self.args = self.args[:1]
        elif self.name == "munmap":
            self.args = self.args[:2]
        elif self.name == "mmap" and len(self.args) == 4:
            self.args += ["-1", "0"]

        self.args[1:] = [str(int(arg, 16)) if "0x" in arg else arg for arg in self.args[1:]]


class LibCall(Call):
    def __init__(self, line: str, syscalls: list = []) -> None:
        Call.__init__(self, line)
        self.syscalls = syscalls.copy()

    def __repr__(self) -> str:
        ret = super().__repr__()
        if self.syscalls:
            ret += "\n  " + "\n  ".join([str(s) for s in self.syscalls])
        return ret


def prettyMmapArgs(mmapSyscall: SysCall):
    mmapSyscall.args[2] = " | ".join(list(map(lambda x: x[0], filter(lambda prot: int(mmapSyscall.args[2]) & prot[1], SysCall.MMAP_PROTS.items()))))
    mmapSyscall.args[3] = " | ".join(list(map(lambda x: x[0], filter(lambda flag: int(mmapSyscall.args[3]) & flag[1], SysCall.MMAP_FLAGS.items()))))


def parseLtraceOutput(ltraceOutput: str):
    # Filter lines that do not contain traced calls
    lines = list(filter(lambda line: any([line.find(tc) != -1 for tc in TRACED_CALLS]), ltraceOutput.splitlines()))
    tracedCalls = []
    unfinishedStack = []
    syscallsBatch = []
    dbgLine = [line[line.rfind('DBG:'):line.rfind(')')] for line in ltraceOutput.splitlines() if 'DBG' in line]

    # Get heap start
    heapStart = int(SysCall(lines[0]).ret, 16)
    # Get exit status
    exitStatus = dbgLine + ltraceOutput.splitlines()[-1:]

    # Store addresses
    heapAddresses = {}
    mappedAddresses = {}

    # Extract libcalls and nested syscalls from output
    for line in lines:
        # Syscalls
        if "SYS" in line:
            # Ignore syscalls made outside a library call
            if unfinishedStack:
                syscallsBatch.append(SysCall(line))
                # Update mmap params
                if syscallsBatch[-1].name == "mmap":
                    prettyMmapArgs(syscallsBatch[-1])
            continue

        # Libcalls
        if "<unfinished" in line:
            unfinishedStack.append(line)
        elif "resumed>" in line:
            if not unfinishedStack:
                print("No call to resume")
                exit(-1)

            newLine = unfinishedStack.pop()
            newLine = newLine[:newLine.find("<")] + line[line.find(">")+1:]

            # Only add top level calls
            if not unfinishedStack:
                tracedCalls.append(LibCall(newLine, syscallsBatch))
                syscallsBatch.clear()
        elif not unfinishedStack:
            # Add top level libcalls
            tracedCalls.append(LibCall(line))

    # Map addresses to relative values
    for libcall in tracedCalls:
        # Syscalls
        for syscall in libcall.syscalls:
            # Mapped addresses
            if syscall.name == "mmap" and syscall.ret not in mappedAddresses:
                index = 1 + len(list(filter(lambda v: "+" not in v, [v for v in mappedAddresses.values()])))
                mappedAddresses[syscall.ret] = f"<mapped-addr{index}>"
            # Heap addresses
            elif syscall.name == "brk" and syscall.ret not in heapAddresses:
                heapAddresses[syscall.ret] = "HeapStart + " + hex(int(syscall.ret, 16) - heapStart)

        # Return values
        if libcall.ret != "<void>" and libcall.ret != "0":
            # Mapped addresses
            if any([s.name == "mmap" for s in libcall.syscalls]):
                key = min([(key, abs(int(libcall.ret, 16) - int(key, 16))) for key in mappedAddresses.keys()], key=lambda x: x[1])[0]
                mappedAddresses[libcall.ret] = mappedAddresses[key]
                offset = int(libcall.ret, 16) - int(key, 16)
                if offset:
                    mappedAddresses[libcall.ret] += f" + {hex(offset)}"
            # Heap addresses
            else:
                heapAddresses[libcall.ret] = "HeapStart + " + hex(int(libcall.ret, 16) - heapStart)

    return tracedCalls, heapAddresses, mappedAddresses, exitStatus


def writeTestOutput(testName, ltraceOutput):
    os.makedirs("out", exist_ok=True)

    tracedCalls, heapAddresses, mappedAddresses, exitStatus = parseLtraceOutput(ltraceOutput)

    # Generate output
    output = "\n".join([str(call) for call in tracedCalls])
    for addr, offset in heapAddresses.items():
        output = output.replace(addr, offset)
    for addr, label in mappedAddresses.items():
        output = output.replace(addr, label)

    # Justify name and args
    output = "\n".join([line.split("=")[0].ljust(90) + line[line.find("="):] for line in output.splitlines()] + exitStatus) + "\n"

    with (open(os.path.join("out", f"{testName}.out"), "w+")) as fout:
        fout.write(output)


def grade(testName: str):
    outPath = os.path.join("out", f"{testName}.out")
    refPath = os.path.join("ref", f"{testName}.ref")

    if not os.path.isfile(outPath):
        print(f"Failed to open {outPath}", file=sys.stderr)
        exit(-1)

    if not os.path.isfile(refPath):
        print(f"Failed to open {refPath}", file=sys.stderr)
        exit(-1)

    with open(outPath, "r") as fout, open(refPath, "r") as fref:
        diffs = list(difflib.unified_diff(fout.readlines(), fref.readlines(), fromfile=outPath, tofile=refPath, lineterm=""))

        if not diffs:
            print(testName.ljust(33) + 24*"." + f" passed ...   {TESTS[testName]}", file=sys.stderr)
            return 1

        print(testName.ljust(33) + 24*"." + " failed ...   0", file=sys.stderr)
        if verbose:
            print(*diffs[:3], sep="\n", file=sys.stderr)
            print(*diffs[3:], sep="", file=sys.stderr)

    return 0


def runTest(testName):
    bin = os.path.join("bin", testName)
    if not os.path.isfile(bin):
        print(f"Failed to open {bin}", file=sys.stderr)
        exit(-1)

    env = os.environ.copy()
    src = os.environ.get("SRC_PATH", "../src")
    env["LD_LIBRARY_PATH"] = src
    proc = Popen(["ltrace", "-F", ".ltrace.conf", "-S", "-x", "os_*", f"{bin}"], stdout=PIPE, stderr=PIPE, env=env)
    _, stderr = proc.communicate()

    ltraceOutput = stderr.decode("ascii")
    writeTestOutput(testName, ltraceOutput)


def parseArgs():
    global TESTS
    global verbose

    if len(sys.argv) > 3:
        print(f"{sys.argv[0]} <test> <-v>", file=sys.stderr)
        exit(-1)
    elif len(sys.argv) == 3:
        if sys.argv[1] == "-v":
            verbose = True
            TESTS = {sys.argv[2]: 0}
        elif sys.argv[2] == "-v":
            verbose = True
            TESTS = {sys.argv[1]: 0}
        else:
            print(f"{sys.argv[0]} <test> <-v>", file=sys.stderr)
            exit(-1)
    elif len(sys.argv) == 2:
        if sys.argv[1] == "-v":
            verbose = True
        else:
            TESTS = {sys.argv[1]: 0}


if __name__ == "__main__":
    parseArgs()

    total = 0
    for testName in TESTS.keys():
        runTest(testName)
        if grade(testName):
            total += TESTS[testName]

    print(f"\nTotal:" + " " * 59 + f" {total}/100", file=sys.stderr)
