// hello: the first test of KolibriOS's Linux personality: the Go runtime
// starts (threads, futexes, memory), goroutines talk over a channel, a
// timer fires, and the result goes to standard output.
package main

import (
	"fmt"
	"os"
	"runtime"
	"time"
)

func main() {
	ch := make(chan int)
	for i := 0; i < 4; i++ {
		go func(n int) {
			s := 0
			for j := 0; j <= n*1000; j++ {
				s += j
			}
			ch <- s
		}(i)
	}
	sum := 0
	for i := 0; i < 4; i++ {
		sum += <-ch
	}
	t0 := time.Now()
	time.Sleep(50 * time.Millisecond)
	buf := make([][]byte, 0)
	for i := 0; i < 200; i++ {
		buf = append(buf, make([]byte, 64<<10))
	}
	runtime.GC()
	fmt.Printf("hello from Go %s on %s/%s: sum %d, slept %v, %d buffers\n",
		runtime.Version(), runtime.GOOS, runtime.GOARCH, sum,
		time.Since(t0).Round(10*time.Millisecond), len(buf))
	os.Exit(0)
}
