#!/usr/bin/env perl
#
# Rewrite pre-theme decoration keys to the grouped decor key names.
#
# Default:
#   cow-migrate-decor.pl < cow.conf > cow.conf.new
#
# In-place:
#   cow-migrate-decor.pl -i cow.conf
#
use strict;
use warnings;
use File::Copy qw(copy move);

my $inplace;
if (@ARGV && $ARGV[0] eq "-i") {
	shift @ARGV;
	$inplace = shift @ARGV;
	die "usage: $0 [-i file]\n" if !defined($inplace) || @ARGV;
}

sub migrate_keys {
	my ($s) = @_;

	$s =~ s/\btitlebar\.button_right2\./titlebar.button.right2./g;
	$s =~ s/\btitlebar\.button_right\./titlebar.button.right./g;
	$s =~ s/\btitlebar\.button_left\./titlebar.button.left./g;
	$s =~ s/\bcolour\.active\b/border.active/g;
	$s =~ s/\bcolour\.inactive\b/border.inactive/g;
	$s =~ s/(?<!border\.)\bborder_style\b/border.style/g;
	$s =~ s/(?<!border\.)\bcorner_length\b/border.corner_length/g;
	$s =~ s/(\bdecor\s+-d\s+\S+\s+)border(\s+)/$1border.width$2/g;
	$s =~ s/(\bset\s+)border(\s+)/$1border.width$2/g;
	$s =~ s/(\bdecor\s+-d\s+\S+\s+)font(\s+)/$1titlebar.font$2/g;
	$s =~ s/(\bset\s+)font(\s+)/$1titlebar.font$2/g;
	$s =~ s/(\bdecor\s+-d\s+\S+\s+)handles(\s+)/$1border.handles$2/g;
	$s =~ s/(\bset\s+)handles(\s+)/$1border.handles$2/g;
	return $s;
}

sub migrate_lines {
	my (@in) = @_;
	my @out;

	for (my $i = 0; $i < @in; $i++) {
		my $line = $in[$i];
		if ($line =~ /^(\s*)decor\s+-d\s+(\S+)\s*\{\s*(?:#.*)?$/) {
			my ($indent, $name) = ($1, $2);
			while (++$i < @in) {
				my $body = $in[$i];
				last if $body =~ /^\s*\}\s*(?:#.*)?$/;
				if ($body =~ /^(\s*)(#?)(\s*)(\S+)(\s+.*)$/) {
					my ($bi, $comment, $sp, $key, $value) =
					    ($1, $2, $3, $4, $5);
					chomp $value;
					my $cmd = $comment eq "#" ? "# decor" : "decor";
					push @out, migrate_keys(
					    "$cmd -d $name $key$value\n");
				} else {
					push @out, migrate_keys($body);
				}
			}
			next;
		}
		push @out, migrate_keys($line);
	}
	return @out;
}

sub read_file {
	my ($path) = @_;
	open my $fh, "<", $path or die "$0: $path: $!\n";
	my @lines = <$fh>;
	close $fh;
	return @lines;
}

sub write_file {
	my ($path, @lines) = @_;
	open my $fh, ">", $path or die "$0: $path: $!\n";
	print {$fh} @lines;
	close $fh;
}

if (defined $inplace) {
	my @lines = migrate_lines(read_file($inplace));
	my $bak = "$inplace.bak";
	copy($inplace, $bak) or die "$0: $inplace -> $bak: $!\n";
	write_file($inplace, @lines);
	exit 0;
}

print migrate_lines(<>);
