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

	$s =~ s/\btitlebar\.button_right2\./titlebar.button.0./g;
	$s =~ s/\btitlebar\.button_right\./titlebar.button.2./g;
	$s =~ s/\btitlebar\.button_left\./titlebar.button.1./g;
	$s =~ s/\btitlebar\.button\.right2\./titlebar.button.0./g;
	$s =~ s/\btitlebar\.button\.right\./titlebar.button.2./g;
	$s =~ s/\btitlebar\.button\.left\./titlebar.button.1./g;
	$s =~ s/(\btitlebar\.button\.[0-9]\.action\s+)close\b/${1}'winops -c'/g;
	$s =~ s/(\btitlebar\.button\.[0-9]\.action\s+)iconify\b/${1}'winops -i'/g;
	$s =~ s/(\btitlebar\.button\.[0-9]\.action\s+)maximize\b/${1}'window-maximize -T'/g;
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

sub is_decor_key {
	my ($key) = @_;

	return 1 if $key =~ /^(?:border\.width|border\.style|border\.corner_length|border\.handles)$/;
	return 1 if $key =~ /^(?:titlebar\.enabled|titlebar\.height|titlebar\.font)$/;
	return 1 if $key =~ /^titlebar\.squeeze(?:\.(?:justify|width|offset|min_width))?$/;
	return 1 if $key =~ /^titlebar\.(?:active_colour|inactive_colour|active_gradient|inactive_gradient|active_image|inactive_image|fg_active|fg_inactive)$/;
	return 1 if $key =~ /^titlebar\.button\.[0-9]\.(?:vector|action|image)$/;
	return 1 if $key =~ /^container\.tab\.(?:active_colour|inactive_colour|active_gradient|inactive_gradient|active_image|inactive_image|fg_active|fg_inactive|sunken)$/;
	return 1 if $key =~ /^border\.(?:active|inactive)(?:\.(?:n|s|e|w|nw|ne|sw|se))?(?:\.gradient|\.image)?$/;
	return 1 if $key =~ /^icon\.(?:size|image|background|foreground|title_relief)$/;
	return 0;
}

sub rule_name {
	my ($line) = @_;

	return $1 if $line =~ /(?:^|\s)-n\s+(\S+)/;
	return $1 if $line =~ /(?:^|\s)-n(\S+)/;
	return undef;
}

sub migrate_rule {
	my (@rule) = @_;
	my $name = rule_name($rule[0]);
	return if !defined $name;

	my @profile;
	for (my $i = 1; $i < @rule; $i++) {
		my $line = migrate_keys($rule[$i]);
		chomp $line;
		$line =~ s/^\s+//;
		$line =~ s/\s*\\\s*$//;
		$line =~ s/\s*\\;\s*$//;
		$line =~ s/\s+$//;
		next if $line eq "";
		return if $line !~ /^set\s+(\S+)\s+(.+)$/;
		my ($key, $value) = ($1, $2);
		return if !is_decor_key($key);
		push @profile, "decor -d $name $key $value\n";
	}
	return if !@profile;

	my $head = $rule[0];
	chomp $head;
	$head =~ s/\s*\\\s*$//;
	$head =~ s/\s+$//;
	push @profile, "$head decor -a $name\n";
	return @profile;
}

sub migrate_lines {
	my (@in) = @_;
	my @out;
	my $need_apply = 0;
	my $has_apply = 0;

	for (my $i = 0; $i < @in; $i++) {
		my $line = $in[$i];
		$has_apply = 1 if $line =~ /^\s*decor\s+-a\s+default(?:\s|$)/;
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
		if ($line =~ /^\s*rule\b/ && $line =~ /\\\s*(?:#.*)?$/) {
			my @rule = ($line);
			while (++$i < @in) {
				push @rule, $in[$i];
				last if $in[$i] !~ /\\\s*(?:#.*)?$/;
			}
			my @migrated = migrate_rule(@rule);
			if (@migrated) {
				push @out, @migrated;
				next;
			}
			push @out, map { migrate_keys($_) } @rule;
			next;
		}
		$line = migrate_keys($line);
		if ($line =~ /^(#?)set\s+(\S+)(\s+.*?)(\n?)$/ &&
		    is_decor_key($2)) {
			my ($comment, $key, $value, $nl) = ($1, $2, $3, $4);
			my $cmd = $comment eq "#" ? "# decor" : "decor";
			$line = "$cmd -d default $key$value$nl";
			$need_apply = 1 if $comment ne "#";
		}
		push @out, $line;
	}
	push @out, "decor -a default\n" if $need_apply && !$has_apply;
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
