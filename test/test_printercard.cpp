// Feeds PrinterCard the listings a printer really sends, and the listings it should
// never send, and demands that the send queue come out the same every time.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../bare/printercard.h"

static int failures = 0;
static const char *scenario = "";

static void check(const bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s: %s\n", scenario, what); ++failures; }
}

static void feed(PrinterCard &card, const std::vector<std::string> &lines) {
  card.begin();
  for (const auto &l : lines)
    card.observe(l.c_str());
}

struct Have { const char *name; uint32_t size; };

static PrinterCardPick pickFrom(const PrinterCard &card, const std::vector<Have> &ours) {
  PrinterCardPick pick;
  pick.begin();
  for (const auto &f : ours)
    pick.consider(card, f.name, f.size);
  return pick;
}

// Exactly what the printer answered to "M20 L" on the bench, carriage returns and all.
static const std::vector<std::string> real = {
  "Begin file list\r",
  "SPOTLI~1/STORE-V2/11868A~2/TM~7.GLO 0 .Spotlight-V100/Store-V2/11868A31-E6B0-4BDE-956E-B56C7BB2AFB0/tmp.Glow\r",
  "PROBE1K.GCO 1036 PROBE1K.GCO\r",
  "DRYRU~15.GCO 6443 dryrun2.gcode\r",
  "BENCH~68.GCO 102423 benchy_test_long_name.gcode\r",
  "End file list\r",
  "ok P31 B31\r",
};

int main() {
  // ------------------------------------------------- the listing from the printer itself
  scenario = "tonight's real listing";
  {
    PrinterCard card;
    feed(card, real);
    check(card.complete(), "it finished");
    check(!card.listing(), "and stopped listening");
    check(card.count() == 3, "three gcode files, the spotlight junk left out");
    check(card.skipped() == 1, "and counted as skipped, not as a parse failure");
    check(card.rejected() == 0, "nothing failed to parse");
    check(strcmp(card.at(0).dos, "PROBE1K.GCO") == 0, "first short name");
    check(strcmp(card.at(2).name, "benchy_test_long_name.gcode") == 0, "third long name");
    check(card.at(2).size == 102423, "third size");
    check(card.holds("dryrun2.gcode", 6443), "found by its long name");
    check(card.holds("DRYRU~15.GCO", 6443), "found by its short name");
    check(card.holds("DRYRUN2.GCODE", 6443), "case does not matter");
    check(!card.holds("dryrun2.gcode", 6444), "but the size does");
    check(!card.holds("tmp.Glow", 0), "the junk is not in the index at all");

    const PrinterCardPick pick = pickFrom(card, {
      { "benchy_test_long_name.gcode", 102423 },
      { "dryrun2.gcode", 6443 },
      { "new_part.gcode", 50000 },
    });
    check(pick.found, "something to send");
    check(strcmp(pick.source, "new_part.gcode") == 0, "the one that is not over there");
    check(strcmp(pick.target, "new_part.gcode") == 0, "a clean name is left alone");
    check(pick.waiting == 1, "and it is the only one waiting");
  }

  // ------------------------------------------------- nothing on the printer's card
  scenario = "an empty card";
  {
    PrinterCard card;
    feed(card, { "Begin file list", "End file list" });
    check(card.complete(), "it finished");
    check(card.count() == 0, "no entries");
    check(!card.holds("anything.gcode", 0), "holds nothing");

    const PrinterCardPick pick = pickFrom(card, { { "a.gcode", 10 }, { "b.gcode", 20 } });
    check(pick.found && strcmp(pick.source, "a.gcode") == 0, "the first by name goes first");
    check(pick.waiting == 2, "both are waiting");

    // The same answer whichever order the module's card hands its files over
    const PrinterCardPick other = pickFrom(card, { { "b.gcode", 20 }, { "a.gcode", 10 } });
    check(strcmp(other.source, pick.source) == 0, "and the order of the asking does not matter");
  }

  scenario = "an empty card and nothing of ours either";
  {
    PrinterCard card;
    feed(card, { "Begin file list", "End file list" });
    const PrinterCardPick pick = pickFrom(card, {});
    check(!pick.found && pick.waiting == 0, "nothing to do");
  }

  // ------------------------------------------------- only other systems' leavings
  scenario = "a card with only junk on it";
  {
    PrinterCard card;
    feed(card, {
      "Begin file list",
      "SPOTLI~1/STORE-V2/TM~7.GLO 0 .Spotlight-V100/Store-V2/tmp.Glow",
      "FSEVEN~1/FSEVEN~1 0 .fseventsd/fseventsd-uuid",
      "TRASHE~1/PS 0 .Trashes/.ps",
      "DS_STORE 6148 .DS_Store",
      "SYSTEM~1 0 System Volume Information",
      "End file list",
    });
    check(card.complete(), "it finished");
    check(card.count() == 0, "not one of them is a file we care about");
    check(card.rejected() == 0, "and none of them looked malformed");
    check(card.skipped() == 5, "all five are accounted for");

    const PrinterCardPick pick = pickFrom(card, { { "part.gcode", 99 } });
    check(pick.found && strcmp(pick.target, "part.gcode") == 0, "our file still has to go");
  }

  // ------------------------------------------------- one bad line among good ones
  scenario = "a malformed line in the middle";
  {
    PrinterCard card;
    feed(card, {
      "Begin file list",
      "FIRST.GCO 100 first.gcode",
      "BROKEN.GCO",                          // no size at all
      "NEXT.GCO notanumber next.gcode",      // size is not a number
      "THIRD.GCO 12x34 third.gcode",         // rubbish glued to the size
      "FOURTH.GCO 99999999999999999999 fourth.gcode",   // a size no card could hold
      " 500 nameless.gcode",                 // no short name
      "SIXTH.GCO -5 sixth.gcode",            // a size that is not a size
      "LAST.GCO 200 last.gcode",
      "End file list",
    });
    check(card.complete(), "it still finished");
    check(card.count() == 2, "the two good lines survived");
    check(card.rejected() == 6, "the six bad ones were thrown away whole");
    check(strcmp(card.at(0).name, "first.gcode") == 0, "the first good one is intact");
    check(strcmp(card.at(1).name, "last.gcode") == 0, "and so is the one after the damage");
    check(card.at(1).size == 200, "with the right size");
    check(card.holds("last.gcode", 200), "and it is found");
    check(!card.holds("BROKEN.GCO", 0), "the wreckage is not in the index");
  }

  scenario = "a listing asked for without the L";
  {
    PrinterCard card;
    feed(card, {
      "Begin file list",
      "PROBE1K.GCO 1036",
      "DRYRU~15.GCO 6443",
      "TM~7.GLO 0",
      "End file list",
    });
    check(card.count() == 2, "the short names answer for themselves");
    check(card.skipped() == 1, "and the non gcode one is still left out");
    check(card.at(0).name[0] == 0, "with no long name claimed");
    check(card.holds("PROBE1K.GCO", 1036), "and it is found by the only name it has");
  }

  // ------------------------------------------------- the listing that never ends
  scenario = "a listing that never says End file list";
  {
    PrinterCard card;
    feed(card, {
      "Begin file list",
      "HALF.GCO 100 half.gcode",
      "OTHER.GCO 200 other.gcode",
    });
    check(!card.complete(), "it is not finished");
    check(card.listing(), "it is still waiting for the rest");
    check(card.count() == 0, "and nothing may be read out of it");
    check(!card.holds("half.gcode", 100), "not even a file we did see go by");

    // Half a listing must not be read as "the printer has nothing", which would send
    // everything we own all over again.
    const PrinterCardPick pick = pickFrom(card, { { "half.gcode", 100 }, { "new.gcode", 7 } });
    check(!pick.found, "nothing is sent on half an answer");
    check(pick.waiting == 0, "and nothing is counted as waiting");

    // The printer answers properly the second time and the index comes good
    feed(card, { "Begin file list", "HALF.GCO 100 half.gcode", "End file list" });
    check(card.complete() && card.count() == 1, "the next listing stands on its own");
    const PrinterCardPick again = pickFrom(card, { { "half.gcode", 100 }, { "new.gcode", 7 } });
    check(again.found && strcmp(again.source, "new.gcode") == 0, "and only the new one goes");
  }

  scenario = "a second Begin wipes the first attempt";
  {
    PrinterCard card;
    feed(card, {
      "Begin file list",
      "GHOST.GCO 1 ghost.gcode",
      "Begin file list",
      "REAL.GCO 2 real.gcode",
      "End file list",
    });
    check(card.count() == 1 && strcmp(card.at(0).name, "real.gcode") == 0, "only the second one counts");
  }

  // ------------------------------------------------- same name, different size
  scenario = "the printer holds the name but not the size";
  {
    PrinterCard card;
    feed(card, { "Begin file list", "PART.GCO 1000 part.gcode", "End file list" });
    const PrinterCardPick pick = pickFrom(card, { { "part.gcode", 2000 } });
    check(pick.found, "a half written file is not a file that is there");
    check(strcmp(pick.target, "part.gcode") == 0, "and it goes under the same name, overwriting it");
    check(pickFrom(card, { { "part.gcode", 1000 } }).found == false, "the right size is left alone");
    check(pickFrom(card, { { "part.gcode", 0 } }).found, "an empty file of ours is not it either");
  }

  // ------------------------------------------------- names at and over the limit
  scenario = "a name at the limit and a name over it";
  {
    char out[PRINTER_CARD_NAME_BYTES];

    const std::string stem(PRINTER_CARD_NAME_MAX - 6, 'a');     // 57 + ".gcode" = 63
    const std::string fits = stem + ".gcode";
    check(fits.size() == PRINTER_CARD_NAME_MAX, "the test's own arithmetic");
    check(printerCardTarget(fits.c_str(), out, sizeof out), "accepted");
    check(strcmp(out, fits.c_str()) == 0, "a name that just fits is untouched");
    check(strlen(out) == PRINTER_CARD_NAME_MAX, "and still fits");

    const std::string over = stem + "a.gcode";                  // one too many
    check(printerCardTarget(over.c_str(), out, sizeof out), "accepted");
    check(strlen(out) <= PRINTER_CARD_NAME_MAX, "the long one was cut to fit");
    check(strcmp(out + strlen(out) - 6, ".gcode") == 0, "the extension survived the cut");
    check(strcmp(out, fits.c_str()) != 0, "and it did not turn into its shorter neighbour");
    check(printerCardIsGcode(out), "it is still gcode as far as the queue is concerned");

    const std::string absurd(400, 'z');
    check(printerCardTarget((absurd + ".gcode").c_str(), out, sizeof out), "accepted");
    check(strlen(out) <= PRINTER_CARD_NAME_MAX, "even an absurd name fits");

    char small[16];
    check(printerCardTarget(over.c_str(), small, sizeof small), "a smaller buffer is honoured");
    check(strlen(small) < sizeof small, "and not overrun");
    check(!printerCardTarget(over.c_str(), out, 4), "a buffer too small to be safe is refused");
  }

  // ------------------------------------------------- two names that shorten to one
  scenario = "two files whose names shorten to the same thing";
  {
    char a[PRINTER_CARD_NAME_BYTES], b[PRINTER_CARD_NAME_BYTES];
    const std::string head(PRINTER_CARD_NAME_MAX, 'x');
    printerCardTarget((head + "one.gcode").c_str(), a, sizeof a);
    printerCardTarget((head + "two.gcode").c_str(), b, sizeof b);
    check(strcmp(a, b) != 0, "cutting them short does not give them one name");

    // The same two through the whole queue, on a printer that already holds the first
    PrinterCard card;
    char line[256];
    snprintf(line, sizeof line, "LONG~1.GCO 10 %s", a);
    feed(card, { "Begin file list", line, "End file list" });
    const PrinterCardPick pick = pickFrom(card, {
      { (head + "one.gcode").c_str(), 10 },
      { (head + "two.gcode").c_str(), 20 },
    });
    check(pick.found, "the second one still has to go");
    check(strcmp(pick.target, b) == 0, "under its own name, not the first one's");
    check(pick.waiting == 1, "and the first is not asked for again");

    // Characters the printer will not take also have to keep files apart
    char p[PRINTER_CARD_NAME_BYTES], q[PRINTER_CARD_NAME_BYTES];
    printerCardTarget("a?b.gcode", p, sizeof p);
    printerCardTarget("a*b.gcode", q, sizeof q);
    check(strcmp(p, q) != 0, "two different illegal names stay two names");
    check(strchr(p, '?') == nullptr && strchr(q, '*') == nullptr, "and neither keeps the bad character");
  }

  scenario = "the names the printer is given";
  {
    struct { const char *in, *want; } clean[] = {
      { "part.gcode",                "part.gcode"                },
      { "/part.gcode",               "part.gcode"                },
      { "Benchy boat v2.gcode",      "Benchy boat v2.gcode"      },
      { "UPPER.GCO",                 "UPPER.GCO"                 },
      { "benchy_test_long_name.gcode", "benchy_test_long_name.gcode" },
    };
    for (auto &c : clean) {
      char out[PRINTER_CARD_NAME_BYTES];
      printerCardTarget(c.in, out, sizeof out);
      check(strcmp(out, c.want) == 0, c.in);
      if (strcmp(out, c.want))
        printf("    got %s, wanted %s\n", out, c.want);
    }

    // Everything the printer would refuse comes back fingerprinted instead
    const char *dirty[] = { "a/b.gcode", "we:ird.gcode", "trailing .gcode", "..gcode",
                            "/\xd0\xab.gcode", "...", "", "/", " .gcode" };
    for (auto &in : dirty) {
      char out[PRINTER_CARD_NAME_BYTES];
      check(printerCardTarget(in, out, sizeof out), in);
      check(out[0] != 0, "never empty");
      check(strlen(out) <= PRINTER_CARD_NAME_MAX, "never too long");
      for (const char *c = out; *c; c++)
        check((unsigned char)*c >= 0x20 && (unsigned char)*c <= 0x7E &&
              !strchr(PRINTER_CARD_FORBIDDEN, *c), "nothing the printer would refuse");
      check(out[strlen(out) - 1] != '.' && out[strlen(out) - 1] != ' ', "no trailing dot or space");
    }

    // The same input always gives the same output, which is what makes the queue derived
    char once[PRINTER_CARD_NAME_BYTES], twice[PRINTER_CARD_NAME_BYTES];
    printerCardTarget("we:ird name.gcode", once, sizeof once);
    printerCardTarget("we:ird name.gcode", twice, sizeof twice);
    check(strcmp(once, twice) == 0, "and it does not change between calls");
  }

  // ------------------------------------------------- what the index must not conclude
  scenario = "a bin file is never judged by this listing";
  {
    PrinterCard card;
    feed(card, { "Begin file list", "PART.GCO 10 part.gcode", "End file list" });
    const PrinterCardPick pick = pickFrom(card, { { "firmware.bin", 500000 }, { "part.gcode", 10 } });
    check(!pick.found, "the missing bin is not taken as a thing to send");
    check(pick.waiting == 0, "nor counted as waiting");
  }

  scenario = "firmware selection checks the exact binary name and size";
  {
    const char *reply = "echo:Now fresh file: Robin_nano.bin\n"
                        "File opened: Robin_nano.bin Size: 298524\r\n"
                        "File selected\n"
                        "ok P31 B31\n";
    check(printerCardOpened(reply, "Robin_nano.bin", 298524), "the full image is selected");
    check(!printerCardOpened(reply, "Robin_nano.bin", 298523), "a short image is refused");
    check(!printerCardOpened(reply, "other.bin", 298524), "a different image is refused");
    check(!printerCardOpened("File opened: Robin_nano.bin Size: 2985240\n", "Robin_nano.bin", 298524),
          "size prefix is refused");
  }

  // ------------------------------------------------- more files than there are slots
  scenario = "more files than slots";
  {
    PrinterCard card;
    std::vector<std::string> lines = { "Begin file list" };
    char buf[64];
    for (int i = 0; i < PRINTER_CARD_SLOTS + 5; i++) {
      snprintf(buf, sizeof buf, "F%04d.GCO %d f%04d.gcode", i, i + 1, i);
      lines.push_back(buf);
    }
    lines.push_back("End file list");
    feed(card, lines);
    check(card.count() == PRINTER_CARD_SLOTS, "the index filled up");
    check(card.total() == PRINTER_CARD_SLOTS + 5, "and counted every file");
    check(card.more() == PRINTER_CARD_SLOTS, "and said where the next window starts");
    check(card.complete(), "it still finished");
    check(!card.holds("f0035.gcode", 36), "a file past the window is not held unasked");

    card.ask(nullptr, PRINTER_CARD_SLOTS);
    feed(card, lines);
    check(card.count() == 5 && card.more() == 0, "the second window holds the rest and is the last");
    check(strcmp(card.at(0).name, "f0032.gcode") == 0, "and starts where the first one stopped");

    card.ask("f0035.gcode", 0);
    feed(card, lines);
    check(card.count() == PRINTER_CARD_SLOTS + 1, "the file asked for got a seat past the window");
    check(card.holds("f0035.gcode", 36), "at its size");
    check(card.find("F0035.GCO") == &card.at(PRINTER_CARD_SLOTS), "and is found by either name");
    check(card.more() == PRINTER_CARD_SLOTS, "the window itself did not move");

    card.ask("f0003.gcode", 0);
    feed(card, lines);
    check(card.count() == PRINTER_CARD_SLOTS && card.holds("f0003.gcode", 4), "one inside the window is not seated twice");

    lines.insert(lines.end() - 1, "TWIN~1.GCO 9 f0035.gcode");
    card.ask("f0035.gcode", 0);
    feed(card, lines);
    bool ambiguous = false;
    check(!card.find("f0035.gcode", &ambiguous) && ambiguous, "two files past the window with one name are ambiguous");
    card.ask(nullptr, 0);
  }

  // ------------------------------------------------- a long name we could never write
  scenario = "a long name longer than anything we can send";
  {
    PrinterCard card;
    const std::string huge(PRINTER_CARD_NAME_BYTES + 10, 'q');
    char line[256];
    snprintf(line, sizeof line, "HUGE~1.GCO 42 %s.gcode", huge.c_str());
    feed(card, { "Begin file list", line, "End file list" });
    check(card.count() == 1, "the entry is kept");
    check(card.oversize() == 1, "and flagged");
    check(card.at(0).name[0] == 0, "with no long name at all, rather than a cut one");
    check(card.holds("HUGE~1.GCO", 42), "its short name still answers");
    char out[PRINTER_CARD_NAME_BYTES];
    printerCardTarget((huge + ".gcode").c_str(), out, sizeof out);
    check(!card.holds(out, 42), "and no name of ours can be mistaken for it");
  }

  scenario = "lines that are not listing lines at all";
  {
    PrinterCard card;
    card.begin();
    check(!card.observe("ok P31 B31"), "before the listing, not ours");
    check(card.observe("Begin file list"), "the opening line is ours");
    check(card.observe("ONE.GCO 1 one.gcode"), "and the entries");
    check(card.observe("End file list"), "and the closing line");
    check(!card.observe("ok P31 B31"), "the ok after it is not");
    check(card.count() == 1, "one file");
  }

  printf(failures ? "\n%d checks failed\n" : "\nall checks passed\n", failures);
  return failures != 0;
}
