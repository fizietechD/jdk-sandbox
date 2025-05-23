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

package jdk.jpackage.test;

import static java.util.stream.Collectors.toCollection;
import static java.util.stream.Collectors.toMap;
import static jdk.jpackage.test.TestBuilder.CMDLINE_ARG_PREFIX;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Deque;
import java.util.List;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.stream.Stream;


public final class Main {

    public static void main(String args[]) throws Throwable {
        main(TestBuilder.build(), args);
    }

    public static void main(TestBuilder.Builder builder, String args[]) throws Throwable {
        boolean listTests = false;
        List<TestInstance> tests = new ArrayList<>();
        try (TestBuilder testBuilder = builder.testConsumer(tests::add).create()) {
            Deque<String> argsAsList = new ArrayDeque<>(List.of(args));
            while (!argsAsList.isEmpty()) {
                var arg = argsAsList.pop();
                TestBuilder.trace(String.format("Parsing [%s]...", arg));

                if ((CMDLINE_ARG_PREFIX + "list").equals(arg)) {
                    listTests = true;
                    continue;
                }

                if (arg.startsWith("@")) {
                    // Command file
                    // @=args will read arguments from the "args" file, one argument per line
                    // @args will read arguments from the "args" file, splitting lines into arguments at whitespaces
                    arg = arg.substring(1);
                    var oneArgPerLine = arg.startsWith("=");
                    if (oneArgPerLine) {
                        arg = arg.substring(1);
                    }

                    var newArgsStream = Files.readAllLines(Path.of(arg)).stream();
                    if (!oneArgPerLine) {
                        newArgsStream.map(line -> {
                            return Stream.of(line.split("\\s+"));
                        }).flatMap(x -> x);
                    }

                    var newArgs = newArgsStream.collect(toCollection(ArrayDeque::new));
                    newArgs.addAll(argsAsList);
                    argsAsList = newArgs;
                    continue;
                }

                boolean success = false;
                try {
                    testBuilder.processCmdLineArg(arg);
                    success = true;
                } catch (Throwable throwable) {
                    TKit.unbox(throwable);
                } finally {
                    if (!success) {
                        TKit.log(String.format("Error processing parameter=[%s]", arg));
                    }
                }
            }
        }

        // Order tests by their full names to have stable test sequence.
        List<TestInstance> orderedTests = tests.stream()
                .sorted(Comparator.comparing(TestInstance::fullName)).toList();

        if (listTests) {
            // Just list the tests
            orderedTests.forEach(test -> System.out.println(String.format(
                    "%s; workDir=[%s]", test.fullName(), test.workDir())));
        }

        orderedTests.stream().collect(toMap(TestInstance::fullName, x -> x, (x, y) -> {
            throw new IllegalArgumentException(String.format("Multiple tests with the same description: [%s]", x.fullName()));
        }));

        if (listTests) {
            return;
        }

        TKit.withExtraLogStream(() -> runTests(orderedTests));
    }

    private static void runTests(List<TestInstance> tests) {
        TKit.runTests(tests);

        final long passedCount = tests.stream().filter(TestInstance::passed).count();
        TKit.log(String.format("[==========] %d tests ran", tests.size()));
        TKit.log(String.format("[  PASSED  ] %d %s", passedCount,
                passedCount == 1 ? "test" : "tests"));

        reportDetails(tests, "[  SKIPPED ]", TestInstance::skipped, false);
        reportDetails(tests, "[  FAILED  ]", TestInstance::failed, true);

        var withSkipped = reportSummary(tests, "SKIPPED", TestInstance::skipped);
        var withFailures = reportSummary(tests, "FAILED", TestInstance::failed);

        if (withFailures != null) {
            throw withFailures;
        }

        if (withSkipped != null) {
            tests.stream().filter(TestInstance::skipped).findFirst().get().rethrowIfSkipped();
        }
    }

    private static long reportDetails(List<TestInstance> tests,
            String label, Predicate<TestInstance> selector, boolean printWorkDir) {

        final Function<TestInstance, String> makeMessage = test -> {
            if (printWorkDir) {
                return String.format("%s %s; workDir=[%s]", label,
                        test.fullName(), test.workDir());
            }
            return String.format("%s %s", label, test.fullName());
        };

        final long count = tests.stream().filter(selector).count();
        if (count != 0) {
            TKit.log(String.format("%s %d %s, listed below", label, count, count
                    == 1 ? "test" : "tests"));
            tests.stream().filter(selector).map(makeMessage).forEachOrdered(
                    TKit::log);
        }

        return count;
    }

    private static RuntimeException reportSummary(List<TestInstance> tests,
            String label, Predicate<TestInstance> selector) {
        final long count = tests.stream().filter(selector).count();
        if (count != 0) {
            final String message = String.format("%d %s %s", count, label, count
                    == 1 ? "TEST" : "TESTS");
            TKit.log(message);
            return new RuntimeException(message);
        }

        return null;
    }
}
