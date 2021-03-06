#!c:/apps/perl/bin/pelr -w

# $Id$

###############################################################################
#
# VC++ message compiler alternative.
# read .mc files and generate corresponding source & include
#
###############################################################################

# disabled; broke build on HP-UX (perl 5.005_02)
#use strict;
#use Data::Dumper;

# file names
my $inFile = shift || die "Usage : mc.pl <input_file>";
my $outHeader = $inFile;
$outHeader =~ s/\.[a-z]+$//;
my $outSource = $outHeader . ".c";
$outHeader .= ".h";

my $type = "DWORD";
my $messageId = 0;
my $comment;
my $symbolicName = "";
my %structData;

open(IN, "<$inFile") || die "input file: $!";
open(OUTH, ">$outHeader") || die "out header: $!";
open(OUTS, ">$outSource") || die "out source: $!";

while(<IN>) {
	chomp;
	my $in = $_;
	my $text = "";

	if ($in eq "") {
		next;
	}

	if ($in =~ /^[;#]+.*$/) {
		print OUTH substr($in, 1) . "\n";
		next; # comment -> skip
	}

	if ($in =~ /^MessageIdTypedef=([A-Z]+)$/i) {
		# typedef
		$type = $1;

		next;
	}

	if ($in =~ /^MessageId=([0-9]*)$/i) {
		if ($1 eq "") {
			$messageId++;
		} else {
			$messageId = $1 + 0;
		}

		next;
	}
	if ($in =~ /^SymbolicName=(.+)$/i) {
		$symbolicName = $1;

		next;
	}
	if ($in =~ /^Language=(.*)$/i) {
		# ignore language
		next;
	}

	# multiline text
	$text = $in;
	while(<IN>) {
		chomp;
		if ($_ ne ".") {
			$text .= "$_\n";
			# end
		} else {
			if ($symbolicName eq "") {
				last;
			}
			print OUTH "//\n// MessageId: $symbolicName\n//\n";
			print OUTH "// MessageText:\n//\n// ";
			$structData{$messageId} = $text;
			$text =~ s/\n/\n\/\//gm;
			print OUTH $text . "\n//\n";

			my $size = 8;
			if ($type =~ /^(WORD|short)$/i) {
				$size = 4;
			}
			printf OUTH ("#define %-30s (($type)0x%0" . $size . "x)\n\n", $symbolicName, $messageId);
			last;
		}
	}
}

print OUTS "/* autogenerated from $inFile */\n\n";
print OUTS "#include <nms_common.h>\n\n";
print OUTS "#include <unicode.h>\n\n";
print OUTS "const TCHAR *g_szMessages[] = {\n";

my @keys = sort { $a <=> $b } (keys %structData);

my $i;
for ($i = 0; $i < $keys[$#keys] + 1; $i++) {
	if (defined $structData{$i}) {
		$structData{$i} =~ s/\"/\\\"/g;
		print OUTS "\t_T(\"" . $structData{$i} . "\")";
	} else {
		print OUTS "\t_T(\"\")";
	}

	if ($i == $keys[$#keys]) {
		print OUTS "\n";
	} else {
		print OUTS ",\n";
	}
}

print OUTS "};\n";
print OUTS "\nunsigned int g_dwNumMessages = $i;\n";

close IN;
close OUTH;
close OUTS;
