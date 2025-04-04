/*
 * Copyright (c) 2019, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package jdk.jfr.api.consumer.filestream;

import java.nio.file.Path;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import jdk.jfr.Event;
import jdk.jfr.Recording;
import jdk.jfr.consumer.EventStream;
import jdk.test.lib.Utils;

/**
 * @test
 * @summary Test EventStream::setOrdered(...)
 * @requires vm.flagless
 * @requires vm.hasJFR
 * @library /test/lib
 * @run main/othervm jdk.jfr.api.consumer.filestream.TestOrdered
 */
public class TestOrdered {

    static class OrderedEvent extends Event {
    }

    static class Emitter extends Thread {
        private final CyclicBarrier barrier;

        public Emitter(CyclicBarrier barrier) {
            this.barrier = barrier;
        }

        @Override
        public void run() {
            OrderedEvent e1 = new OrderedEvent();
            e1.commit();
            try {
                barrier.await();
            } catch (Exception e) {
                e.printStackTrace();
                throw new Error("Unexpected exception in barrier");
            }
            Instant timestamp = Instant.now();
            // Wait for clock to increment
            while (Instant.now().equals(timestamp)) {
                ;
            }
            OrderedEvent e2 = new OrderedEvent();
            e2.commit();
        }
    }

    private static final int THREAD_COUNT = 4;
    private static final boolean[] BOOLEAN_STATES = { false, true };

    public static void main(String... args) throws Exception {
        Path p = makeUnorderedRecording();

        testSetOrderedTrue(p);
        testSetOrderedFalse(p);
    }

    private static void testSetOrderedTrue(Path p) throws Exception {
        for (boolean reuse : BOOLEAN_STATES) {
            System.out.println();
            System.out.println("Testing: testSetOrderedTrue reuse = " + reuse);
            AtomicReference<Instant> timestamp = new AtomicReference<>(Instant.MIN);
            AtomicBoolean unordered = new AtomicBoolean(false);
            try (EventStream es = EventStream.openFile(p)) {
                es.setReuse(reuse);
                es.setOrdered(true);
                es.onEvent(e -> {
                    Instant endTime = e.getEndTime();
                    printTimestamp(endTime);
                    if (endTime.isBefore(timestamp.get())) {
                        unordered.set(true);
                    }
                    timestamp.set(endTime);
                });
                es.start();
            }
            if (unordered.get()) {
                throw new Exception("Events are not ordered! Reuse = " + reuse);
            }
        }
    }

    private static void testSetOrderedFalse(Path p) throws Exception {
        for (boolean reuse : BOOLEAN_STATES) {
            System.out.println();
            System.out.println("Testing: testSetOrderedFalse reuse = " + reuse);
            AtomicReference<Instant> timestamp = new AtomicReference<>(Instant.MIN);
            AtomicBoolean unordered = new AtomicBoolean(false);
            try (EventStream es = EventStream.openFile(p)) {
                es.setReuse(reuse);
                es.setOrdered(false);
                es.onEvent(e -> {
                    Instant endTime = e.getEndTime();
                    printTimestamp(endTime);
                    if (endTime.isBefore(timestamp.get())) {
                        unordered.set(true);
                    }
                    timestamp.set(endTime);
                });
                es.start();
                if (!unordered.get()) {
                    throw new Exception("Expected at least some events to be out of order! Reuse = " + reuse);
                }
            }
        }
    }

    private static void printTimestamp(Instant timestamp) {
        long seconds = timestamp.getEpochSecond();
        long nanos = timestamp.getNano();
        System.out.println(timestamp + " seconds = " + seconds + " nanos = " + nanos);
    }

    private static Path makeUnorderedRecording() throws Exception {
        while (true) {
            CyclicBarrier barrier = new CyclicBarrier(THREAD_COUNT + 1);
            try (Recording r = new Recording()) {
                r.start();
                List<Emitter> emitters = new ArrayList<>();
                for (int i = 0; i < THREAD_COUNT; i++) {
                    Emitter e = new Emitter(barrier);
                    e.start();
                    emitters.add(e);
                }
                // Thread buffers should now have one event each
                barrier.await();
                // Add another event to each thread buffer, so
                // events are bound to come out of order when they
                // are flushed
                for (Emitter e : emitters) {
                    e.join();
                }
                r.stop();
                Path p = Utils.createTempFile("recording", ".jfr");
                r.dump(p);
                // Order is only guaranteed within a segment.
                int segments = countSegments(p);
                if (segments < 2) {
                    return p;
                }
                System.out.println("File contains more than one segment (" + segments + "). Retrying.");
            }
        }
    }

    private static int countSegments(Path file) throws Exception {
        AtomicInteger segments = new AtomicInteger();
        try (EventStream es = EventStream.openFile(file)) {
            es.onFlush(segments::incrementAndGet);
            es.start();
            return segments.get();
        }
    }
}
