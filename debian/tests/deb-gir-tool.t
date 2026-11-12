#!/usr/bin/perl
# Copyright 2025 Simon McVittie
# SPDX-License-Identifier: LGPL-2.1-or-later

use strict;
use warnings;

use File::Temp qw(tempdir);
use IPC::Run3;
use String::ShellQuote;
use Test::More qw(no_plan);
use autodie;

$ENV{DEBUG_INVOCATION} = '1';

sub run_it {
    my $argv = shift;
    my @stdout;
    my @stderr;
    my $stdout;
    my %options = @_;

    diag("command: ".shell_quote(@$argv));

    if ($options{binary}) {
        run3 $argv, \undef, \$stdout, \@stderr;
        my $escaped = $stdout;
        $escaped =~ s{([^-A-Za-z0-9_/.])}{sprintf '\x%02x', ord $1}ge;
        diag("stdout (binary): $escaped");
        @stdout = ($stdout);
    }
    else {
        run3 $argv, \undef, \@stdout, \@stderr;
        foreach my $line (@stdout) {
            diag("stdout: $line");
        }
    }

    foreach my $line (@stderr) {
        diag("stderr: $line");
    }

    diag("wait status: $?");
    return @stdout;
}

my @out;
my $tmp = tempdir(CLEANUP => 1);

run_it(['deb-gir-tool', '--help']);
is($?, 0);

run_it(['deb-gir-tool', 'solve-everything']);
is($?, 2 << 8);

@out = run_it([
    'env',
    'DEB_HOST_MULTIARCH=aperture-glados-eabi',
    'GI_GIR_PATH=/nonexistent',
    'deb-gir-tool', 'print-search-path',
]);
is($?, 0);
is_deeply(
    \@out,
    [
        "/usr/share/gir-1.0\n",
        "/usr/lib/aperture-glados-eabi/gir-1.0\n",
        "/usr/share/gir-1.0\n",
    ],
);

@out = run_it([
    'env', 'DEB_HOST_MULTIARCH=aperture-glados-eabi',
    'deb-gir-tool', 'print-search-path', '--zero-terminated',
], binary => 1);
is($?, 0);
like($out[0], qr{\0\z}s);
$out[0] =~ s/\0\z//;
unlike($out[0], qr{\0\z}s);
is_deeply(
    [split /\0/, $out[0]],
    [
        '/usr/share/gir-1.0',
        '/usr/lib/aperture-glados-eabi/gir-1.0',
        '/usr/share/gir-1.0',
    ],
);

@out = run_it([
    'deb-gir-tool',
    'find',
]);
is($?, 2 << 8);
is_deeply(\@out, []);

@out = run_it([
    'deb-gir-tool',
    'find',
    'certainly not a real module-123',
]);
is($?, 1 << 8);
is_deeply(\@out, []);

do {
    open(my $fh, '>', "$tmp/badness-1.gir");
    print $fh '<include></include>';
    close $fh;
};

@out = run_it([
    'env',
    "GI_GIR_PATH=$tmp",
    'deb-gir-tool',
    'find',
    '--upstream-paths',
    'badness-1',
]);
is($?, 0);
is_deeply(\@out, ["$tmp/badness-1.gir\n"]);

@out = run_it([
    'env',
    "GI_GIR_PATH=$tmp",
    'deb-gir-tool',
    'find',
    '--upstream-paths',
    '--with-dependencies',
    'badness-1',
]);
is($?, 1 << 8);

do {
    open(my $fh, '>', "$tmp/badness-2.gir");
    print $fh '<include name="GLib"/>';
    close $fh;
};

@out = run_it([
    'env',
    "GI_GIR_PATH=$tmp",
    'deb-gir-tool',
    'find',
    '--upstream-paths',
    '--with-dependencies',
    'badness-2',
]);
is($?, 1 << 8);

do {
    open(my $fh, '>', "$tmp/badness-3.gir");
    print $fh '<include version="123"/>';
    close $fh;
};

@out = run_it([
    'env',
    "GI_GIR_PATH=$tmp",
    'deb-gir-tool',
    'find',
    '--upstream-paths',
    '--with-dependencies',
    'badness-3',
]);
is($?, 1 << 8);

mkdir("$tmp/badness-4.gir", 0755);
@out = run_it([
    'env',
    "GI_GIR_PATH=$tmp",
    'deb-gir-tool',
    'find',
    '--upstream-paths',
    '--with-dependencies',
    'badness-4',
]);
is($?, 1 << 8);

SKIP: {
    skip 'Gio-2.0 not in expected location', 2 unless -e '/usr/share/gir-1.0/Gio-2.0.gir';

    @out = run_it([
        'deb-gir-tool',
        'find',
        'Gio-2.0',
    ]);
    is($?, 0);
    is_deeply(\@out, ["/usr/share/gir-1.0/Gio-2.0.gir\n"]);

    @out = run_it([
        'deb-gir-tool',
        'find',
        '--zero-terminated',
        'Gio-2.0',
    ]);
    is($?, 0);
    is_deeply(\@out, ["/usr/share/gir-1.0/Gio-2.0.gir\0"]);
}

@out = run_it([
    'deb-gir-tool',
    'find',
    'GLib-2.0',
]);
is($?, 0);

if (-e '/usr/share/gir-1.0/GLib-2.0.gir') {
    is_deeply(\@out, ["/usr/share/gir-1.0/GLib-2.0.gir\n"]);
}
else {
    like(join('', @out), qr{\A/usr/lib/[^/]+/gir-1\.0/GLib-2\.0\.gir\n\z}s);
}

my $glib = $out[0];
$glib =~ s/\n\z//;

# This assumes that GObject's only dependency is GLib,
# and it is in /usr/share/gir-1.0.
# This test will need changing if GLib ever changes that fact.
@out = run_it([
    'deb-gir-tool',
    'find',
    '--with-dependencies',
    'GObject-2.0',
]);
is($?, 0);
is_deeply(\@out, ["/usr/share/gir-1.0/GObject-2.0.gir\n", "$glib\n"]);

@out = run_it([
    'deb-gir-tool',
    'find',
    '--if-exists',
    'certainly not a real module-123',
    'GLib-2.0',
    'also not a real module-456',
    'GObject-2.0',
]);
is($?, 0);
is_deeply(
    \@out,
    [
        "$glib\n",
        "/usr/share/gir-1.0/GObject-2.0.gir\n",
    ],
);

ok(! -e "$tmp/GLib-2.0.gir");
ok(! -e "$tmp/GObject-2.0.gir");
@out = run_it([
    'deb-gir-tool',
    'find',
    '--target-directory', $tmp,
    '--with-dependencies',
    'GObject-2.0',
]);
is($?, 0);
is_deeply(\@out, []);
ok(-f "$tmp/GLib-2.0.gir");
ok(-f "$tmp/GObject-2.0.gir");

@out = run_it([
    'deb-gir-tool',
    'find',
    '--target-directory', $tmp,
    'GObject-2.0',
]);
is($?, 1 << 8, '--target-directory should fail rather than overwriting');
is_deeply(\@out, []);

do {
    open(my $fh, '>', "$tmp/GObject-2.0.gir");
    close $fh;
};
ok(! -s "$tmp/GObject-2.0.gir");

@out = run_it([
    'deb-gir-tool',
    'find',
    '--keep',
    '--target-directory', $tmp,
    'GObject-2.0',
]);
is($?, 0, 'Existing file should be kept');
ok(! -s "$tmp/GObject-2.0.gir", 'Existing file should be kept');
is_deeply(\@out, []);

@out = run_it([
    'deb-gir-tool',
    'find',
    '--overwrite',
    '--target-directory', $tmp,
    'GObject-2.0',
]);
is($?, 0, 'Existing file should be overwritten');
ok(-s "$tmp/GObject-2.0.gir", 'Existing file should be overwritten');
is_deeply(\@out, []);

done_testing;

# vim:set sw=4 sts=4 et:
