/*
 * greasy.c  —  CS4328 Project 2: Greasy Cards
 *
 * Usage: ./greasy <seed> <num_players> <chips_per_bag>
 *
 * Compile: gcc -Wall -Wextra -o greasy greasy.c -lpthread
 */

#include <stdio.h>        // provides printf, fprintf, fopen, fclose, fflush, perror, FILE
#include <stdlib.h>       // provides atoi, rand, srand, NULL
#include <stdarg.h>       // provides va_list, va_start, va_end for variadic functions
#include <pthread.h>      // provides all pthread_* threading primitives
#include <string.h>       // included for potential string utilities (not directly used here)
/* #include <unistd.h> */   /* usleep — uncomment during dev only */

/* ═══════════════════════════════════════════════════════════════
   Defines
═══════════════════════════════════════════════════════════════ */
#define DECK_SIZE    52    // a standard deck has 52 cards
#define MAX_PLAYERS  20    // maximum number of players allowed in a game
#define MAX_HAND     52    // a player can hold at most a full deck's worth of cards

/* ═══════════════════════════════════════════════════════════════
   Structs
═══════════════════════════════════════════════════════════════ */
typedef struct {           // begins the definition of the Deck type
    int cards[DECK_SIZE];  // array holding the integer card values (1–13)
    int top;               // index of the next card to draw (head of circular buffer)
    int count;             // how many cards remain undrawn in the deck
    int size;              // total capacity of the circular buffer (always DECK_SIZE)
} Deck;                    // end of Deck struct

typedef struct {               // begins the definition of the ChipBag type
    int chips;                 // number of chips currently remaining in the bag
    int bag_number;            // which bag number we are currently on (starts at 1)
    int chips_per_bag;         // how many chips each new bag starts with
    pthread_mutex_t lock;      // mutex to protect concurrent access to chip bag fields
} ChipBag;                     // end of ChipBag struct

typedef struct {        // begins the definition of the Player type
    int id;             // player number, 1-indexed
    int hand[MAX_HAND]; // array of cards currently in this player's hand
    int hand_size;      // how many cards the player currently holds
} Player;               // end of Player struct

/* ═══════════════════════════════════════════════════════════════
   Globals — shared state across all threads
═══════════════════════════════════════════════════════════════ */
Deck    deck;                  // the single shared deck of cards used by all threads
ChipBag bag;                   // the single shared chip bag used by all players
Player  players[MAX_PLAYERS];  // array of all player structs, indexed 0 to n_players-1
int     n_players;             // actual number of players for this game session

int greasy_card;               // the target card value that wins the round
int current_turn;              // id of the player who should act next
int round_winner;              // id of the player who won the round; -1 = no winner yet
int round_over;                // flag: set to 1 once a winner has been declared
int current_round;             // which round number is currently being played
int dealer_this_round;         // id of the player acting as dealer in the current round

pthread_mutex_t game_lock;     // master lock guarding all round-state globals above
pthread_cond_t  turn_cond;     // condition variable signaled whenever current_turn changes
pthread_cond_t  round_cond;    // condition variable signaled on round start or round end

int             barrier_count; // counts how many threads have reached the end-of-round barrier
pthread_mutex_t barrier_lock;  // protects barrier_count from concurrent modification
pthread_cond_t  barrier_cond;  // woken by the last thread to arrive at the barrier

FILE           *log_file;      // file handle for game.log output
pthread_mutex_t log_lock;      // serializes all writes to log_file across threads
pthread_mutex_t rand_lock;     // serializes all rand() calls to keep the RNG sequence deterministic

/* ═══════════════════════════════════════════════════════════════
   safe_rand — thread-safe wrapper around rand().
   glibc's rand() protects individual calls but the SEQUENCE is
   non-deterministic when multiple threads call it concurrently.
   Serializing via rand_lock makes the sequence fully reproducible
   for a given seed regardless of thread scheduling.
═══════════════════════════════════════════════════════════════ */
int safe_rand(void) {                   // returns one pseudo-random integer in a thread-safe manner
    pthread_mutex_lock(&rand_lock);     // acquire exclusive access to the RNG before calling rand()
    int r = rand();                     // generate the next random number in the global sequence
    pthread_mutex_unlock(&rand_lock);   // release the RNG lock so other threads can use it
    return r;                           // return the generated value to the caller
}                                       // end of safe_rand

/* ═══════════════════════════════════════════════════════════════
   card_name — convert integer 1-13 to display string
   Returns a pointer to a string literal; thread-safe (read-only).
═══════════════════════════════════════════════════════════════ */
const char *card_name(int v) {                              // takes a card value 1-13 and returns its printable name
    static const char *names[] = {                          // static lookup table indexed by card value
        NULL, "A","2","3","4","5","6","7","8","9","10","J","Q","K"  // index 0 unused; 1=Ace, 11=Jack, 12=Queen, 13=King
    };                                                      // end of names array initializer
    if (v >= 1 && v <= 13) return names[v];                // return the name if value is in the valid 1–13 range
    return "?";                                             // return "?" for any out-of-range or invalid value
}                                                           // end of card_name

/* ═══════════════════════════════════════════════════════════════
   log_write — thread-safe printf to game.log
   Works exactly like printf; can be called from any thread.
   Lock ordering: game_lock (optional) -> log_lock
═══════════════════════════════════════════════════════════════ */
void log_write(const char *fmt, ...) {              // variadic log function; fmt is a printf-style format string
    va_list args;                                   // declare the variable-argument list handle
    va_start(args, fmt);                            // initialize args to point at the first variadic argument after fmt
    pthread_mutex_lock(&log_lock);                  // acquire log_lock to prevent interleaved output from other threads
    vfprintf(log_file, fmt, args);                  // write the formatted message to the log file
    fflush(log_file);                               // flush immediately so the file is always current on disk
    pthread_mutex_unlock(&log_lock);                // release log_lock so other threads can write
    va_end(args);                                   // clean up the va_list to avoid undefined behavior
}                                                   // end of log_write

/* ═══════════════════════════════════════════════════════════════
   log_deck — write current deck contents to log
   Caller MUST hold game_lock so the deck doesn't change mid-print.
═══════════════════════════════════════════════════════════════ */
void log_deck(void) {                                               // prints all remaining deck cards to the log file
    pthread_mutex_lock(&log_lock);                                  // acquire log_lock for exclusive file access
    fprintf(log_file, "DECK:");                                     // print the "DECK:" label prefix to start the line
    for (int i = 0; i < deck.count; i++) {                         // iterate over every remaining card in the deck
        int idx = (deck.top + i) % deck.size;                      // compute the wrapped index into the circular buffer
        fprintf(log_file, " %s", card_name(deck.cards[idx]));      // print the card's display name with a leading space
    }                                                               // end of deck-iteration loop
    fprintf(log_file, "\n");                                        // terminate the DECK line with a newline
    fflush(log_file);                                               // flush to ensure the entry is written to disk immediately
    pthread_mutex_unlock(&log_lock);                                // release the log lock for other threads
}                                                                   // end of log_deck

/* ═══════════════════════════════════════════════════════════════
   log_hand — print a player's hand to the log
   - hand_size == 1  ->  "PLAYER X: hand Y"
   - hand_size  > 1  ->  "PLAYER X: hand (Y,Z)<>Greasy card is G"
   Caller MUST hold game_lock (reads greasy_card and player hand).
═══════════════════════════════════════════════════════════════ */
void log_hand(Player *p) {                                                  // prints player p's current hand to the log
    pthread_mutex_lock(&log_lock);                                          // acquire log_lock for thread-safe file access
    if (p->hand_size == 1) {                                                // single-card hand: use the simpler format without greasy card suffix
        fprintf(log_file, "PLAYER %d: hand %s\n",                          // write "PLAYER X: hand Y\n" to the log
                p->id, card_name(p->hand[0]));                              // substitute player id and the single card's display name
    } else {                                                                // two-or-more card hand: parenthesized list plus greasy card annotation
        fprintf(log_file, "PLAYER %d: hand (", p->id);                     // open the parenthesized card list with the player id prefix
        for (int i = 0; i < p->hand_size; i++) {                           // iterate over every card currently in the player's hand
            if (i) fprintf(log_file, ",");                                  // print a comma separator before each card after the first
            fprintf(log_file, "%s", card_name(p->hand[i]));                // print this card's display name
        }                                                                   // end of hand-printing loop
        fprintf(log_file, ") <> Greasy card is %s\n",                       // close the list and append the greasy card annotation
                card_name(greasy_card));                                    // print the current round's greasy card name
    }                                                                       // end of hand-size branch
    fflush(log_file);                                                       // flush so the entry is immediately persisted to disk
    pthread_mutex_unlock(&log_lock);                                        // release the log lock for other threads
}                                                                           // end of log_hand

/* ═══════════════════════════════════════════════════════════════
   init_deck — fill the deck with 52 cards (called once in main)
═══════════════════════════════════════════════════════════════ */
void init_deck(Deck *d) {                       // initializes deck d with 4 suits × 13 values = 52 cards
    int k = 0;                                  // k is the write index into d->cards[]
    for (int suit = 0; suit < 4; suit++)        // iterate over 4 suits (suits are identical in this game)
        for (int val = 1; val <= 13; val++)     // iterate over values Ace(1) through King(13) for each suit
            d->cards[k++] = val;                // store the card value and advance the write index
    d->top   = 0;                               // set the draw pointer to the beginning of the array
    d->count = DECK_SIZE;                       // all 52 cards are now available to draw
    d->size  = DECK_SIZE;                       // record the total circular-buffer capacity
}                                               // end of init_deck

/* ═══════════════════════════════════════════════════════════════
   shuffle_deck — Fisher-Yates shuffle; refills to 52 cards first.
   Called by the dealer at the start of every round.
   Caller MUST hold game_lock.
═══════════════════════════════════════════════════════════════ */
void shuffle_deck(Deck *d) {                        // refills and randomly shuffles deck d in place
    /* Refill */
    int k = 0;                                      // reset the fill index to rebuild the deck array from scratch
    for (int suit = 0; suit < 4; suit++)            // loop over 4 suits to regenerate every card
        for (int val = 1; val <= 13; val++)         // loop over 13 values per suit
            d->cards[k++] = val;                    // write the card value into the array and advance the fill index

    /* Shuffle */
    for (int i = DECK_SIZE - 1; i > 0; i--) {      // Fisher-Yates: scan from the last position down to index 1
        int j   = safe_rand() % (i + 1);            // pick a uniformly random index j in [0, i]
        int tmp = d->cards[i];                      // save the card at position i before overwriting it
        d->cards[i] = d->cards[j];                  // move the randomly chosen card into position i
        d->cards[j] = tmp;                          // put the saved card into position j, completing the swap
    }                                               // end of Fisher-Yates shuffle loop
    d->top   = 0;                                   // reset the draw pointer to the start of the freshly shuffled deck
    d->count = DECK_SIZE;                           // all 52 cards are available to draw again
}                                                   // end of shuffle_deck

/* ═══════════════════════════════════════════════════════════════
   deck_draw — take the top card from the deck and return it.
   Caller MUST hold game_lock.
═══════════════════════════════════════════════════════════════ */
int deck_draw(Deck *d) {                            // removes and returns the card at the top of the deck
    int card = d->cards[d->top];                    // read the card value at the current draw position
    d->top   = (d->top + 1) % d->size;             // advance the draw pointer, wrapping around the circular buffer
    d->count--;                                     // decrement the count of cards remaining in the deck
    return card;                                    // return the drawn card value to the caller
}                                                   // end of deck_draw

/* ═══════════════════════════════════════════════════════════════
   deck_put_back — place a discarded card at the end of the deck.
   Uses the circular buffer: tail = top + count (mod size).
   Caller MUST hold game_lock.
═══════════════════════════════════════════════════════════════ */
void deck_put_back(Deck *d, int card) {             // appends a discarded card to the tail of the circular buffer
    int tail       = (d->top + d->count) % d->size; // compute the index one past the last remaining card
    d->cards[tail] = card;                          // store the returned card at the tail position
    d->count++;                                     // increment the count to include the newly returned card
}                                                   // end of deck_put_back

/* ═══════════════════════════════════════════════════════════════
   init_bag — initialize the chip bag (called once in main).
═══════════════════════════════════════════════════════════════ */
void init_bag(ChipBag *b, int chips_per_bag) {      // sets up the chip bag with an initial full supply
    b->chips_per_bag = chips_per_bag;               // store the configured chips-per-bag capacity for future refills
    b->chips         = chips_per_bag;               // start the first bag completely full
    b->bag_number    = 1;                           // this is the first bag opened (bag #1)
    pthread_mutex_init(&b->lock, NULL);             // initialize the bag's mutex with default attributes
}                                                   // end of init_bag

/* ═══════════════════════════════════════════════════════════════
   eat_chips — player eats 1-5 chips after their turn.
   Opens a new bag if the current one is empty.
   MUST be called OUTSIDE game_lock to avoid deadlock.
   Acquires bag.lock internally.
═══════════════════════════════════════════════════════════════ */
void eat_chips(Player *p) {                                                 // lets player p consume a random number of chips from the bag
    int amount = (safe_rand() % 5) + 1;                                     // randomly pick 1–5 chips to eat this turn

    pthread_mutex_lock(&bag.lock);                                          // acquire the bag lock before reading or modifying chip state

    if (bag.chips == 0) {                                                   // the current bag is empty; open a new one
        bag.bag_number++;                                                   // increment to the next bag number
        bag.chips = bag.chips_per_bag;                                      // refill to a full bag's worth of chips
        log_write("PLAYER %d: opens bag %d\n", p->id, bag.bag_number);    // log that this player opened a new bag
        log_write("BAG: %d chips\n", bag.chips);                           // log the new bag's starting chip count
    }                                                                       // end of new-bag-open branch

    if (amount > bag.chips)                                                 // can't eat more chips than remain in the bag
        amount = bag.chips;                                                 // clamp the requested amount to what is available

    bag.chips -= amount;                                                    // subtract the eaten chips from the bag's remaining count
    log_write("PLAYER %d: eats %d chips\n", p->id, amount);               // log how many chips the player ate
    log_write("BAG: %d Chips left\n", bag.chips);                          // log the updated chip count after eating

    pthread_mutex_unlock(&bag.lock);                                        // release the bag lock so other threads can access the bag
}                                                                           // end of eat_chips

/* ═══════════════════════════════════════════════════════════════
   init_sync — initialize all mutexes and condition variables.
   Called once in main() before any threads are created.
═══════════════════════════════════════════════════════════════ */
void init_sync(void) {                                      // initializes every synchronization primitive used by the game
    pthread_mutex_init(&game_lock,    NULL);                // set up the master game-state mutex with default attributes
    pthread_mutex_init(&log_lock,     NULL);                // set up the log-file write mutex with default attributes
    pthread_mutex_init(&barrier_lock, NULL);                // set up the round-end barrier mutex with default attributes
    pthread_mutex_init(&rand_lock,    NULL);                // set up the RNG serialization mutex with default attributes
    pthread_cond_init (&turn_cond,    NULL);                // set up the condition variable used to signal turn changes
    pthread_cond_init (&round_cond,   NULL);                // set up the condition variable used to signal round start/end
    pthread_cond_init (&barrier_cond, NULL);                // set up the condition variable used by the round-end barrier
    barrier_count = 0;                                      // no threads have reached the barrier yet at program start
}                                                           // end of init_sync

/* ═══════════════════════════════════════════════════════════════
   cleanup_sync — destroy all mutexes and condition variables.
   Called in main() after all threads have been joined.
═══════════════════════════════════════════════════════════════ */
void cleanup_sync(void) {                               // releases all pthread synchronization resources
    pthread_mutex_destroy(&game_lock);                  // destroy the master game-state mutex
    pthread_mutex_destroy(&log_lock);                   // destroy the log-file write mutex
    pthread_mutex_destroy(&barrier_lock);               // destroy the barrier mutex
    pthread_mutex_destroy(&rand_lock);                  // destroy the RNG serialization mutex
    pthread_mutex_destroy(&bag.lock);                   // destroy the chip bag mutex (initialized separately in init_bag)
    pthread_cond_destroy(&turn_cond);                   // destroy the turn-change condition variable
    pthread_cond_destroy(&round_cond);                  // destroy the round start/end condition variable
    pthread_cond_destroy(&barrier_cond);                // destroy the barrier condition variable
}                                                       // end of cleanup_sync

/* ═══════════════════════════════════════════════════════════════
   check_match — return 1 if any card in hand equals greasy_card.
   Caller MUST hold game_lock (reads greasy_card).
═══════════════════════════════════════════════════════════════ */
int check_match(Player *p) {                            // returns 1 if player p holds the greasy card, 0 otherwise
    for (int i = 0; i < p->hand_size; i++)             // scan every card currently in the player's hand
        if (p->hand[i] == greasy_card) return 1;       // found a match: this player can win the round
    return 0;                                           // no card in hand matched the greasy card
}                                                       // end of check_match

/* ═══════════════════════════════════════════════════════════════
   discard_random — remove a random card from hand and put it at
   the end of the deck.
   Caller MUST hold game_lock.
═══════════════════════════════════════════════════════════════ */
void discard_random(Player *p) {                                                    // removes one random card from p's hand and returns it to the deck
    int i    = safe_rand() % p->hand_size;                                          // choose a uniformly random index within the current hand
    int card = p->hand[i];                                                          // save the value of the card that will be discarded

    log_write("PLAYER %d: discards %s at random\n", p->id, card_name(card));       // log the discard event before modifying the hand

    /* Swap with last element and shrink — O(1) removal */
    p->hand[i] = p->hand[p->hand_size - 1];                                        // overwrite the discarded slot with the last card to fill the gap
    p->hand_size--;                                                                 // shrink the hand size by one to reflect the removed card

    deck_put_back(&deck, card);                                                     // return the discarded card to the tail of the shared deck
}                                                                                   // end of discard_random

/* ═══════════════════════════════════════════════════════════════
   next_player — return the id of the next player in rotation,
   skipping the dealer for this round.
   Requires: dealer_this_round is set, n_players >= 2.
═══════════════════════════════════════════════════════════════ */
int next_player(int from) {                             // returns the id of the player who plays immediately after player 'from'
    int next = (from % n_players) + 1;                 // advance one position in the 1-indexed circular rotation
    if (next == dealer_this_round)                      // the dealer does not participate as a player this round; skip them
        next = (next % n_players) + 1;                 // advance one more position past the dealer
    return next;                                        // return the id of the next active player
}                                                       // end of next_player

/* ═══════════════════════════════════════════════════════════════
   barrier_wait — synchronize all n threads at the end of a round.
   Every thread calls this; the last to arrive wakes all others.
═══════════════════════════════════════════════════════════════ */
void barrier_wait(void) {                                           // blocks until all n_players threads have finished the current round
    pthread_mutex_lock(&barrier_lock);                              // acquire the barrier lock before modifying barrier_count
    barrier_count++;                                                // register this thread as having reached the barrier

    if (barrier_count == n_players) {                              // this is the last thread to arrive at the barrier
        /* Last thread — reset counter and wake everyone */
        barrier_count = 0;                                          // reset the counter so it is ready for the next round's barrier
        pthread_cond_broadcast(&barrier_cond);                      // wake all threads sleeping on barrier_cond
    } else {                                                        // this is not the last thread; must wait for the others
        /* Not last — wait until last thread resets counter to 0 */
        while (barrier_count != 0)                                  // loop to guard against spurious wakeups from pthread_cond_wait
            pthread_cond_wait(&barrier_cond, &barrier_lock);        // atomically release barrier_lock and sleep until the broadcast
    }                                                               // end of last-vs-not-last branch

    pthread_mutex_unlock(&barrier_lock);                            // release the barrier lock before returning to the caller
}                                                                   // end of barrier_wait

/* ═══════════════════════════════════════════════════════════════
   do_dealer — everything the dealer does for one round:
     1. Shuffle deck and pick the Greasy Card
     2. Deal one card to each non-dealer player
     3. Reset chip bag
     4. Set round state and signal round start
     5. Wait for someone to win
     6. Log "Round ends" and print win/loss to console
═══════════════════════════════════════════════════════════════ */
void do_dealer(Player *me, int round) {                                                     // executes all dealer responsibilities for the given round number

    pthread_mutex_lock(&game_lock);                                                         // acquire game_lock before modifying any shared round-state globals

    dealer_this_round = me->id;                                                             // record which player is dealing so next_player() can skip them

    /* 1. Shuffle the deck */
    shuffle_deck(&deck);                                                                    // refill and randomize the deck for a fresh round
    log_write("PLAYER %d: shuffles deck for round %d\n", me->id, round);                  // log the shuffle event
    log_deck();                                                                             // log the full shuffled order before any cards are drawn

    /* 2. Draw the Greasy Card */
    greasy_card = deck_draw(&deck);                                                         // remove the top card from the deck and make it this round's target
    log_write("PLAYER %d: greasy card is %s\n", me->id, card_name(greasy_card));          // log which card is the greasy (winning) card for this round

    /* 3. Deal one card to every non-dealer player and log each hand */
    for (int i = 0; i < n_players; i++) {                                                  // clear every hand before dealing the new round
        players[i].hand_size = 0;                                                           // the dealer keeps an empty hand because they are not playing
        if (players[i].id == me->id)                                                       // skip the dealer during the starting deal
            continue;                                                                       // move on to the next player without drawing a card
        players[i].hand[0] = deck_draw(&deck);                                             // draw from the top of the deck and place it in slot 0 of their hand
        players[i].hand_size = 1;                                                           // each non-dealer starts the round with exactly one card
    }                                                                                       // end of dealing loop
    for (int i = 0; i < n_players; i++) {                                                  // log every non-dealer player's starting hand after all dealing is done
        if (players[i].id == me->id)                                                       // the dealer has no hand to log this round
            continue;                                                                       // skip the dealer hand entry
        log_write("PLAYER %d: hand %s\n",                                                  // format: "PLAYER X: hand Y\n"
                  players[i].id, card_name(players[i].hand[0]));                           // substitute player id and their single starting card name
    }                                                                                       // end of starting-hand logging loop

    /* 4. Reset round state */
    round_winner  = -1;                                                                     // -1 indicates no winner has been declared yet
    round_over    =  0;                                                                     // 0 means the round is still in progress
    current_round = round;                                                                  // publish the round number so waiting player threads can detect round start

    /* First turn: player immediately after the dealer */
    current_turn = next_player(me->id);                                                     // set the first active player (skipping the dealer)

    pthread_mutex_unlock(&game_lock);                                                       // release game_lock before touching bag.lock (lock-ordering rule)

    /* 5. Reset chip bag for this round (outside game_lock) */
    pthread_mutex_lock(&bag.lock);                                                          // acquire bag lock to safely reset the chip supply for the new round
    bag.chips      = bag.chips_per_bag;                                                     // refill the bag to its configured capacity
    bag.bag_number = 1;                                                                     // reset to bag #1 for the new round
    log_write("BAG: %d chips\n", bag.chips);                                               // log the freshly refilled bag's chip count
    pthread_mutex_unlock(&bag.lock);                                                        // release the bag lock

    /* 6. Signal round start — wake all waiting player threads */
    pthread_mutex_lock(&game_lock);                                                         // re-acquire game_lock before broadcasting on its condition variables
    pthread_cond_broadcast(&turn_cond);                                                     // wake any thread sleeping on turn_cond in case it missed the round start
    pthread_cond_broadcast(&round_cond);                                                    // wake player threads blocked in do_player waiting for current_round to advance

    /* 7. Dealer waits here until a winner is declared */
    while (!round_over)                                                                     // loop to handle spurious wakeups; check round_over each time
        pthread_cond_wait(&round_cond, &game_lock);                                         // sleep until a player broadcasts round_cond after setting round_over = 1

    log_write("PLAYER %d: Round ends\n", me->id);                                         // log the round-end event from the dealer's perspective
    pthread_mutex_unlock(&game_lock);                                                       // release game_lock; round state is now final

    /* 8. Print result to console */
    if (round_winner == me->id)                                                             // check whether the dealer itself won (normally the dealer doesn't play)
        printf("Player %d won round %d\n", me->id, round);                                // print win message to stdout for the dealer
    else                                                                                    // the winner was one of the non-dealer players
        printf("Player %d lost round %d\n", me->id, round);                               // dealer prints their own loss message to stdout
}                                                                                           // end of do_dealer

/* ═══════════════════════════════════════════════════════════════
   do_player — everything a non-dealer player does for one round:
     - Wait for the round to start
     - Loop: wait for turn → draw → match or discard → eat chips
     - When round ends (winner found), log loss and exit
═══════════════════════════════════════════════════════════════ */
void do_player(Player *me, int round) {                             // executes all non-dealer player actions for one round

    /* Wait until the dealer has finished setting up this round */
    pthread_mutex_lock(&game_lock);                                 // acquire game_lock to safely read current_round
    while (current_round < round)                                   // loop until the dealer has published this round number
        pthread_cond_wait(&round_cond, &game_lock);                 // sleep until the dealer broadcasts round_cond after completing setup
    pthread_mutex_unlock(&game_lock);                               // release game_lock; round setup is complete and safe to proceed

    while (1) {                                                     // main gameplay loop; runs until this player wins or someone else does
        pthread_mutex_lock(&game_lock);                             // acquire game_lock before reading or changing any turn/round state

        /* Wait until it's our turn OR the round ends */
        while (current_turn != me->id && !round_over)              // re-check both conditions to guard against spurious wakeups
            pthread_cond_wait(&turn_cond, &game_lock);             // sleep until current_turn changes or round_over is set to 1

        /* Someone else won while we were waiting */
        if (round_over) {                                           // another player won while this thread was waiting for its turn
            log_write("PLAYER %d: lost round %d\n", me->id, round); // log that this player did not win this round
            pthread_mutex_unlock(&game_lock);                       // release game_lock before exiting the function
            printf("Player %d lost round %d\n", me->id, round);   // print loss result to stdout
            break;                                                  // exit the gameplay loop; this player is done for this round
        }                                                           // end of early-exit-on-round-over branch

        /* ── Our turn ── */

        /* Log hand before drawing */
        log_hand(me);                                               // log this player's current hand before they draw a new card

        /* Draw a card */
        int drawn = deck_draw(&deck);                               // take the top card from the shared deck
        me->hand[me->hand_size++] = drawn;                         // append the drawn card to the player's hand and increment hand size
        log_write("PLAYER %d: draws %s\n", me->id, card_name(drawn)); // log the card that was just drawn

        /* Check for a match with the Greasy Card */
        if (check_match(me)) {                                      // the player's hand now contains the greasy card — they win
            /* Win: log winning hand, declare winner, wake everyone */
            log_hand(me);                                           // log the winning hand in the "(cards)<>Greasy card is G" format
            log_write("PLAYER %d: wins round %d\n", me->id, round); // log the win event to the game log
            round_winner = me->id;                                  // record this player as the round winner
            round_over   = 1;                                       // signal that the round is finished
            pthread_cond_broadcast(&turn_cond);                     // wake any player threads sleeping on turn_cond
            pthread_cond_broadcast(&round_cond);                    // wake the dealer thread sleeping in do_dealer's wait loop
            pthread_mutex_unlock(&game_lock);                       // release game_lock before performing I/O and eating chips
            printf("Player %d won round %d\n", me->id, round);    // print the win result to stdout
            eat_chips(me);                                          // winner celebrates by eating chips (called outside game_lock)
            break;                                                  // exit the gameplay loop; this player won the round
        }                                                           // end of win branch

        /* No match: discard a random card, log hand and deck, advance turn */
        discard_random(me);                                         // remove a random card from hand and return it to the deck
        log_hand(me);                                               // log the player's hand after the discard
        log_deck();                                                 // log the remaining deck state for debugging/auditing

        current_turn = next_player(me->id);                        // advance the turn to the next active player
        pthread_cond_broadcast(&turn_cond);                        // wake the next player's thread which is waiting on turn_cond
        pthread_mutex_unlock(&game_lock);                          // release game_lock before eating chips (lock-ordering rule)

        /* Eat chips between turns — OUTSIDE game_lock */
        eat_chips(me);                                              // consume a random number of chips while the next player takes their turn

        /* Loop back and wait for our next turn */
    }                                                               // end of main gameplay while(1) loop
}                                                                   // end of do_player

/* ═══════════════════════════════════════════════════════════════
   player_func — thread entry point.
   Loops over all n rounds; calls do_dealer or do_player each round;
   synchronizes with all threads via barrier_wait at round end.
═══════════════════════════════════════════════════════════════ */
void *player_func(void *arg) {                          // pthread-compatible entry point; arg is a pointer to this thread's Player struct
    Player *me = (Player *)arg;                         // cast the void* argument back to a typed Player pointer

    for (int round = 1; round <= n_players; round++) {  // play one round per player; each player acts as dealer exactly once
        if (me->id == round)                            // this player's id matches the round number: it is their turn to deal
            do_dealer(me, round);                       // act as dealer: shuffle, deal, wait for a winner
        else                                            // all other players participate as non-dealers this round
            do_player(me, round);                       // act as non-dealer: wait for turn, draw, match or discard

        /* All n threads must reach here before any proceeds to round+1 */
        barrier_wait();                                 // synchronize all threads at the end of the round before starting the next one
    }                                                   // end of round loop

    return NULL;                                        // return NULL as required by the pthread thread-function signature
}                                                       // end of player_func

/* ═══════════════════════════════════════════════════════════════
   main — parse args, initialize, create threads, join, clean up.
═══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {                                          // program entry point; argc = argument count, argv = argument strings

    if (argc != 4) {                                                        // exactly 3 arguments are required: seed, num_players, chips_per_bag
        fprintf(stderr, "Usage: %s <seed> <num_players> <chips_per_bag>\n", // print usage instructions to stderr
                argv[0]);                                                   // argv[0] is the program name as invoked
        return 1;                                                           // exit with error code 1 to signal invalid usage
    }                                                                       // end of argument-count check

    int seed          = atoi(argv[1]);                                      // parse the RNG seed from the first command-line argument
    n_players         = atoi(argv[2]);                                      // parse the player count from the second command-line argument
    int chips_per_bag = atoi(argv[3]);                                      // parse chips-per-bag capacity from the third command-line argument

    if (n_players < 2 || n_players > MAX_PLAYERS) {                        // enforce the supported player-count range [2, MAX_PLAYERS]
        fprintf(stderr, "num_players must be between 2 and %d\n", MAX_PLAYERS); // explain the valid range to the user on stderr
        return 1;                                                           // exit with error code 1
    }                                                                       // end of player-count validation
    if (chips_per_bag < 1) {                                               // each bag must hold at least one chip to be valid
        fprintf(stderr, "chips_per_bag must be >= 1\n");                   // explain the constraint to the user on stderr
        return 1;                                                           // exit with error code 1
    }                                                                       // end of chips-per-bag validation

    /* Seed the RNG — all randomness derives from this */
    srand(seed);                                                            // seed the C standard library RNG so results are reproducible for a given seed

    /* Initialize shared data */
    init_deck(&deck);                                                       // populate the global deck with 52 unshuffled cards
    init_bag(&bag, chips_per_bag);                                         // set up the global chip bag with the configured per-bag capacity
    init_sync();                                                            // initialize all mutexes and condition variables before creating threads

    /* Open log file */
    log_file = fopen("game.log", "w");                                     // create (or overwrite) game.log in write mode in the current directory
    if (!log_file) {                                                        // fopen returns NULL if the file could not be opened
        perror("fopen game.log");                                           // print the OS-level error message to stderr
        return 1;                                                           // exit with error code 1
    }                                                                       // end of log-file-open check

    /* Initialize player structs */
    for (int i = 0; i < n_players; i++) {                                  // initialize each player's struct before any threads are created
        players[i].id        = i + 1;                                      // assign 1-based player ids (player 1, 2, … n_players)
        players[i].hand_size = 0;                                           // each player starts with an empty hand before dealing begins
    }                                                                       // end of player-initialization loop

    /* Initialize round state */
    current_round     = 0;                                                  // 0 means no round has started yet; round 1 begins when thread 1 runs
    current_turn      = 0;                                                  // 0 means no player's turn has been set yet
    round_winner      = -1;                                                 // -1 indicates no winner has been declared
    round_over        =  0;                                                 // 0 means the round has not ended
    dealer_this_round =  0;                                                 // 0 means no dealer has been assigned yet

    /* Create one thread per player */
    pthread_t threads[MAX_PLAYERS];                                         // array to store thread handles for later joining
    for (int i = 0; i < n_players; i++)                                    // create exactly one thread for each player
        pthread_create(&threads[i], NULL, player_func, &players[i]);       // spawn a thread with player_func as the entry point, passing the player's struct

    /* Wait for all threads to finish */
    for (int i = 0; i < n_players; i++)                                    // join every player thread in creation order
        pthread_join(threads[i], NULL);                                     // block until thread i has exited; the return value is discarded

    /* Clean up */
    fclose(log_file);                                                       // flush all buffered output and close the game.log file
    cleanup_sync();                                                         // destroy all mutexes and condition variables to free OS resources

    printf("Game over.\n");                                                 // print a final message to stdout indicating the game has completed
    return 0;                                                               // exit successfully with code 0
}                                                                           // end of main
