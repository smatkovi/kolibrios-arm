// The Linux personality's random numbers (docs/rng.md), started by the
// kernel (kolibri.run=/bin/rngtest, tests/rng/qemu.py):
//
//	rngtest        getrandom with each flag, the flags refused as Linux
//	               refuses them, /dev/urandom and /dev/random, crypto/rand,
//	               FIPS 140-2's tests on four blocks from getrandom
//	rngtest wait   with a generator that has no seed (kolibri.rngjitter=off,
//	               no source): GRND_NONBLOCK gives EAGAIN, GRND_INSECURE
//	               gives bytes, a blocking getrandom and a read of
//	               /dev/urandom wait
//
// Each result is a line "rngtest: ok ..." or "rngtest: FAIL ...", the last
// "rngtest: done, N failures".
package main

import (
	"bytes"
	"crypto/rand"
	"fmt"
	"os"
	"syscall"
	"time"
	"unsafe"
)

const (
	sysGetrandom = 278
	grndNonblock = 1
	grndRandom   = 2
	grndInsecure = 4
)

var failures int

func check(ok bool, format string, a ...any) {
	s := "ok"
	if !ok {
		s = "FAIL"
		failures++
	}
	fmt.Printf("rngtest: %s %s\n", s, fmt.Sprintf(format, a...))
}

func getrandom(b []byte, flags uintptr) (int, syscall.Errno) {
	n, _, e := syscall.Syscall(sysGetrandom, uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)), flags)
	return int(n), e
}

// FIPS 140-2's monobit, poker, runs and long run tests on 20000 bits
func fips(b []byte) string {
	ones, f := 0, make([]int, 16)
	runs := [2][6]int{}
	longest, run, last := 0, 0, -1
	for _, v := range b {
		f[v>>4]++
		f[v&15]++
		for j := 7; j >= 0; j-- {
			bit := int(v>>j) & 1
			ones += bit
			if bit == last {
				run++
				continue
			}
			if last >= 0 {
				runs[last][min(run, 6)-1]++
				longest = max(longest, run)
			}
			last, run = bit, 1
		}
	}
	runs[last][min(run, 6)-1]++
	longest = max(longest, run)
	sum := 0
	for _, c := range f {
		sum += c * c
	}
	px := 16*sum - 25000000
	lo := []int{2315, 1114, 527, 240, 103, 103}
	hi := []int{2685, 1386, 723, 384, 209, 209}
	bad := ""
	if !(ones > 9725 && ones < 10275) {
		bad += " monobit"
	}
	if !(px > 10800 && px < 230850) {
		bad += " poker"
	}
	for x := 0; x < 2; x++ {
		for l := 0; l < 6; l++ {
			if runs[x][l] < lo[l] || runs[x][l] > hi[l] {
				bad += " runs"
				x, l = 2, 6
			}
		}
	}
	if longest >= 26 {
		bad += " long-run"
	}
	return bad
}

func seeded() {
	a, b := make([]byte, 64), make([]byte, 64)
	n, e := getrandom(a, 0)
	check(n == 64 && e == 0, "getrandom(64, 0): %d %v", n, e)
	n, e = getrandom(b, 0)
	check(n == 64 && !bytes.Equal(a, b), "a second getrandom differs")
	n, e = getrandom(b, grndNonblock)
	check(n == 64 && e == 0, "GRND_NONBLOCK: %d %v", n, e)
	n, e = getrandom(b, grndRandom)
	check(n == 64 && e == 0, "GRND_RANDOM: %d %v", n, e)
	n, e = getrandom(b, grndInsecure)
	check(n == 64 && e == 0, "GRND_INSECURE: %d %v", n, e)
	_, e = getrandom(b, grndInsecure|grndRandom)
	check(e == syscall.EINVAL, "GRND_INSECURE|GRND_RANDOM refused: %v", e)
	_, e = getrandom(b, 8)
	check(e == syscall.EINVAL, "an unknown flag refused: %v", e)
	big := make([]byte, 300000)
	n, e = getrandom(big, 0)
	check(n == len(big) && e == 0, "getrandom of 300000 bytes: %d", n)
	for _, dev := range []string{"/dev/urandom", "/dev/random"} {
		f, err := os.Open(dev)
		if err != nil {
			check(false, "%s: %v", dev, err)
			continue
		}
		u := make([]byte, 4096)
		n, err := f.Read(u)
		f.Close()
		check(n == 4096 && err == nil && !bytes.Equal(u[:64], make([]byte, 64)), "%s: %d bytes", dev, n)
	}
	k := make([]byte, 32)
	_, err := rand.Read(k)
	check(err == nil, "crypto/rand: %v", err)
	pass := 0
	for i := 0; i < 4; i++ {
		blk := make([]byte, 2500)
		getrandom(blk, 0)
		if bad := fips(blk); bad == "" {
			pass++
		} else {
			fmt.Printf("rngtest: block %d fails%s\n", i, bad)
		}
	}
	check(pass >= 3, "FIPS 140-2: %d of 4 blocks from getrandom pass", pass)
}

func unseeded() {
	b := make([]byte, 32)
	_, e := getrandom(b, grndNonblock)
	check(e == syscall.EAGAIN, "no seed: GRND_NONBLOCK gives EAGAIN: %v", e)
	n, e := getrandom(b, grndInsecure)
	check(n == 32 && e == 0, "no seed: GRND_INSECURE gives bytes: %d %v", n, e)
	f, err := os.OpenFile("/dev/urandom", os.O_RDONLY|syscall.O_NONBLOCK, 0)
	if err == nil {
		_, err = syscall.Read(int(f.Fd()), b)
		check(err == syscall.EAGAIN, "no seed: /dev/urandom with O_NONBLOCK gives EAGAIN: %v", err)
		f.Close()
	}
	done := make(chan int, 2)
	go func() {
		n, _ := getrandom(make([]byte, 16), 0)
		done <- n
	}()
	go func() {
		g, err := os.Open("/dev/urandom")
		if err == nil {
			n, _ := g.Read(make([]byte, 16))
			done <- n
		}
	}()
	select {
	case n := <-done:
		check(false, "no seed: a blocking read returned %d bytes", n)
	case <-time.After(3 * time.Second):
		check(true, "no seed: a blocking getrandom and a read of /dev/urandom still wait after 3 s")
	}
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "wait" {
		unseeded()
	} else {
		seeded()
	}
	fmt.Printf("rngtest: done, %d failures\n", failures)
	os.Exit(failures)
}
