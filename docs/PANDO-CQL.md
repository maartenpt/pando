# Pando Corpus Query Language

The native query language of pando is called pando-CQL. These guidelines give a general introduction to pando-CQL (henceforth simply CQL), with example queries illustrated against the **sample corpus** shipped with this repository: `test/data/sample.conllu`.
See the documentation on how to install and use the sample corpus. 

## Token Queries

The base query in CQL searches for a word, with optionally one or more restrictions. A token is represented with two square brackets, with the restrictions inside. So if we want to look for a word with the lemma "book", we express that in CQL as `[lemma="book"]`, which will find all examples of *book* and *books*, as a noun, but also *book*, *books*, *booked* and *booking* as as a verb.

In the sample corpus, English *book* examples live in the **book** genre (`text_genre="Book"`) and **English** (`text_lang="English"`). To narrow to that slice, combine region-style attributes with lemma: `[lemma="book" & text_genre="Book" & text_lang="English"]`.

If we only want to find the verbs, we can add multiple restrictions an a token, putting a **&** between the restrictions to say that all restrictions should be met (a **|** would instead match if any of the restrictions is met). So: `[lemma="book" & upos="VERB" & text_genre="Book" & text_lang="English"]` will match only examples of *book*, *books*, etc. as a verb in that subcorpus.

The original CQL is a purely sequence-based query language, in which we can look for sequences of tokens, by putting several tokens next to each other. So the query `a:[upos="DET"] [lemma="book"] :: a.text_genre = "Book" & a.text_lang = "English"` looks for any determiner in the corpus that is directly followed by the word (lemma) book, **within** English texts in the book genre, and will hence find examples like *the book*, *some books*, etc. there.

## Token repetitions

Since CQL is purely sequence based, the last query above will not find examples like *the green book*, which is why CQL introduces token repetitions operators, places directly after the closing bracket. For instance, `[]?` means any optional token. So the query `a:[upos="DET"] []? [lemma="book"] :: a.text_genre = "Book" & a.text_lang = "English"` will still find *the book*, but allows a single token in between the determiner and book, so *the green book*, *some interesting book*, etc. But it will also include probably unintented results like *some people book* (their hotel early).

There are four different repititions, depending on how many tokens are allowed or required:

| symbol | interpretation |
| ----- | ----- |
| []? | 0 or 1 tokens |
| []* | 0 or more tokens |
| []+ | 1 or more tokens |
| []{m,n} | between m and n tokens |

## Dependency relations

Our query `[upos="DET"] []* [lemma="book"]` was probably intended to look for any word *book* modified by a determiner, something that you cannot express in CWB/CQL. That is why pando-CQL introduces the option to look for dependency relations. With that, our query becomes `a:[upos="DET"] < [lemma="book"] :: a.text_genre = "Book" & a.text_lang = "English"`, which will find any determiner governed by the word *book*, so it will not longer find *some people book*, but it will find the determiner and the noun in *some ugly, and sometimes even very, very ugly, green books*. The relation can also be expressed the other way around: `[lemma="book"] > [upos="DET"] :: match.text_genre = "Book" & match.text_lang = "English"`. It will find any relation, so if we instead look for `[upos="ADJ"] < [lemma="book"] :: match.text_genre = "Book" & match.text_lang = "English"`, it will not only find *green book*, but also the same word in *The book that was on the table was green.*.

Dependency and sequence relations can be combined, with the token being interpreted by the symbol between them, so `[upos="DET"] [upos="ADJ"] < [lemma="book"] :: match.text_genre = "Book" & match.text_lang = "English"` requires the adjective to be to the right of the determiner, as well as the adjective to be governed by the word *book*. And it is possible to negate dependencies: `[upos="DET"] !< [lemma="book"] :: match.text_genre = "Book" & match.text_lang = "English"` for occurrences of *book* without a determiner.

Intead of using sequence notation, it is also possible to define dependency relations as token restriction, in a notation similar to that used in PML-TQ. In that case, to look for a noun modified by a determiner, we specify inside the token that we are looking for a child that is a determiner: `[upos="NOUN" & child [upos="DET"] ]`. This notation has the advantage that you can specify multiple children, and still have the option to furthermore look for words to the left or the right. And there are more option in the token-restriction notation: you can look not only for `child`, but also for `parent`, `ancestor`, `descendant`, or `sibling`.

Also for depenencies as token restrictions, we can use negations: `[upos="VERB" & not child [deprel="nsubj"]]` to look for any verbs without a nominal subject. 

## Regions

Like in CWB-CQL, we can also restrict our query by properties of the region it is in - the sentence, the text, the paragraph, etc. The notation for the attributes of regions is the name of the region, an underscore, and the name of the attribute, so the genre of the text is named `text_genre`, which can have values in the same way positions (token) attributes do. We can use those in a number of different ways:

| example | type  | explanation |
| ----- | -----  | ----- |
| `[ text_genre="Book"]` | token-restriction | the token is in a given region+attribute |
| `<text genre="Book"> []` | region-hook | look only for tokens directly following the start of a region+attribute (attribute is optional) |
| `a:[] :: match.text_genre = "Book" & a.text_lang = "English"` | global condition | the named token lies in a text with those attributes |
| `[] within text_genre="Book"` | within restriction | the whole match has to appear inside a region+attribute (attribute is optional) |


## Within and containing

Like in Manatee, we can add a (single) token restriction on the the `within` clause - if we want to look for an interjection in a sentence that needs to contain the word *cat* we can do that by saying `[upos="INTJ"] within s having [form="cat"] :: match.text_genre = "Book" & match.text_lang = "English"` (Universal Dependencies tag `INTJ`; in the sample corpus that sentence is in the English book subcorpus).

Inversely, we can also say that a the match need to contain a given region, which we can express with `containing name`, meaning that our result has to contain an entire region named `<name>`. In CWB-CQL, a common example of that is to require something to contain an entire NP, but syntactic elements like NP are not regions, and in dependency parsed corpora, they are implicitly encoded in dependency subtrees. That is why in pando you can say `containing subtree [upos="NOUN"]`, which will require the result to contain all leaves of a dependency subtree heading by an noun, which is the dependency equivalent of a noun phrase. 

Both within and containing can also be negated: `[] not within s`.

## Named tokens and aligned corpora

You can give a name to the tokens in your query, so that you can then refer back to it: `a:[] b:[] :: a.form = b.form`. But in contrast to CWB-CQL, where the life-span of names is restricted to the query, names in pando-CQL are persistent, so that you can refer to them in subsequent grouping queries or other queries.

The fact that names are persistnt also allows us to use named tokens to search through aligned corpora, if the alignment is done in the set-up used by TEITOK. That settings models alignment by having a shared attribute, in TEITOK that is `tuid` for *translation unit identifier*, although it can also be used to align version of a text in the same language. How that works is best explained with an example.

If we have an aligned corpus English-Dutch, with a tuid on sentences, we can look for a word in our English text in the first query, say the word *property*. We do that by a regular token query, that we can then name `eng` for convenience: `eng:[lemma="property"] :: match.text_genre = "Book" & match.text_lang = "English"`. (The English demo document is in the book genre; the Dutch side is only `text_lang="Dutch"` with no book genre.) Then in a subsequent query, we can look for nouns in Dutch, but only in sentences that are translations of the English sentences we found, which we can find by making sure that the tuid attribute of the sentence that the `eng` token is in (`eng.s_tuid`) is the same as the tuid of the sentence we are using for the translation: `nld:[upos="NOUN"] :: match.text_lang = "Dutch" & eng.s_tuid = nld.s_tuid`. This way, we get the nouns in translation of the English sentences containing the word *property*.

For **token-level** alignment (matching the Dutch word that translates a specific English word), the same `tuid` must appear on both tokens. In CoNLL-U, store that in the **MISC** column, e.g. `tuid=align-prop-01-w2` on both *property* and *eigendom* in the sample corpus, in addition to the sentence-level `# tuid` comment. You can filter the Dutch hits to the translation layer with `[text_lang="Dutch"]` or `:: match.text_lang = "Dutch"`.

## Regular expressions

To look for patterns in words, CQL allow regular expressions in token restrictions. In regular expressions, you can look for optional letters, repeated letters, etc. In pando-CQL, regular expressions use the format that is made popular by Python: `[form = /.*tion/]`. Contrary to CWB-CQL, regular expressions are used in their natural meaning, and not as "word-bound matches". So where in CWB, `[form = ".*tion"]` will match all and only words ending in *-tion*, in pando-CQL, `[form = /.*tion/]` will give exactly the same matches as `[form = /tion/]`, since the optional characters at the beginning do not have any effect in this particular query, and there is no requirement that the *tion* matches at the end of the word, only that it matches the word. To mimic the query behaviour of CWB, the query has to be explictly bound to the word boundaries: `[form = /^.*tion$/]`, which is more naturally expressed as `[form = /tion$/]`.


## Matching strategy flags

It is often desirable to look for words case or diacritics insensitively. In CQL, that is done by putting a flag after the condition: `[ form = "the" %c ] :: match.text_genre = "book" & match.text_lang = "English"` will match both *the* and *The* in that subcorpus, and `[ form = "een" %d ] :: match.text_lang = "Dutch"` will match both *een* and *één* in the Dutch document. 

The case flags only work on literal comparisons, not on regular expressions, to get a case-sensitive regular expression, you have to use regular expression flags like `/(?i)^hello/` to search for hello case-insensitive at the beginning of the word.

## Named queries and frequencies

Like in CWB, you can name queries, so that you can later refer back to them in results:  `Matches = a:[lemma="book" & text_genre="Book" & text_lang="English"]`. In fact, pando always names each query, where if no explicit name is provided, the query will be implicitly called `Last`.

After a query has been executed, frequency results can be obtained from it, in which the name of the query, and the name(s) of the query tokens can be used: `Matches = a:[lemma="book" & text_genre="Book" & text_lang="English"]; count Matches by a.form;` will look for all occurences of the lemma *book*, store it as *Matches* and then provide the frequency of each form in which it is used in corpus. Counting can be done by more than one attribute: `count Matches by a.form, a.text_century` will give an overview of which form of *book* was used in which frequency in each century - and not only that, but if one of the attributes used in the grouping is not a token attribute but a region attribute, the system will futhermore return the *relative frequency*, that is to say in the example, how frequent each form is in each century, relative to the total number of tokens for that century.

By using named frequencies relations, we can count English hits with `Matches = a:[lemma="property" & text_genre="Book" & text_lang="English"]; count Matches by a.form`. For the Dutch translation in the sample, the aligned lemma shares the same word-level `tuid` (see **MISC**): `b:[lemma="eigendom" & text_lang="Dutch" & tuid="align-prop-01-w2"]`. Joining those with `b.tuid=a.tuid` in a parallel query is the usual way to tabulate translation pairs.

## Corpus position 

It is possible to use comparisons between the corpus positions of tokens to ensure that one is after the other - for sequential searches that is not that relevant, but we can look for all modifying adjectives (in Spanish or French, where both occur) that appear before a noun, by making comparing their positions: `a:[upos="NOUN" & text_lang="French"] > b:[upos="ADJ"] :: a > b` (pre-nominal adjective in the French sentence in the sample). For Spanish, noun-before-adjective order is illustrated in the same corpus with `match.text_lang = "Spanish"` and a different dependency pattern.

## Controlling the output

By default, if no futher queries are provided, pando will return results as keywords-in-context (KWIC), that is to say, the matches in the middle, with some words to the left and some words to the right of the result, which can also be explicitly triggered with the command `cat Matches`, where *Matches* is the name of the query.  

To get more control over the output, we can also explicitly choose what should appear in the output: `tabulate Matches a.form, a.lemma, a.upos, a.text_century, a.text_lang`

will give a table with those columns for query token *a* in a query named *Matches*, including the **text** region’s language and century when exposed as `text_lang` / `text_century` on tokens.

## Collocations

After keywords-in-context, collocations are one of the most popular queries in corpus processors. Pando offers the option to directly calculate collocations in the CQL, similar to the way *count* works - we search for something, and then look for the most characteristic (the most unexpectedly frequent) words in its context. So we can search for the most typical words to appear next to the verb *book* as follows: `[upos="VERB" & lemma="book"]; coll by lemma`. This will list the most prominent words appearing next to the verb *book* as counted by lemma. 

Apart from the left/right context, we can also look in the dependency context to see which words most typically modify a given word. This is done with `dcoll`, which takes a list of dependency relations to traverse. Relations can be specific deprel labels like `amod` or `nsubj` (which select children with that label), or the special keywords `head` (go up to the governor), `children` (all children), or `descendants` (full subtree). If no relation is specified, all children are collected by default.

For example: `[upos="NOUN" & lemma="book"]; dcoll amod by lemma` gives the most typical adjectival modifiers of the noun *book*. To see which words govern *book*: `dcoll head by lemma`. Relations can be combined: `dcoll head, amod by lemma` collects both heads and amod children.

For multi-token queries, use a named token as anchor with dot notation: `a:[upos="DET"] [upos="NOUN" & lemma="book"]; dcoll a.amod by lemma` — this anchors the dependency collocation on the determiner rather than the default first matched token.

The CQL commands only define the search, not the way the results are produced in the output. In pando, the context window, the collocation measures (logdice, mi, mi3, tscore, ll, dice), the minimum frequency, the maximum number of results, etc. are provided on the command line with flags like `--window`, `--measures`, `--min-freq`, and `--max-items`.

## Keyness

Keyness identifies words that are statistically overrepresented in a subcorpus compared to a reference. Since named queries in pando effectively define subcorpora, keyness works much like `count` and `coll` — you run a query that selects the tokens of interest, then ask which words stand out.

For example, to find keywords in the French subcorpus compared to the rest of the corpus: `[text_lang="French"]; keyness by lemma`. Without an explicit reference, the comparison is against the complement — all corpus positions not in the target query.

To compare two specific subcorpora, use the `vs` keyword: `Fr = [text_lang="French"]; En = [text_lang="English"]; keyness Fr vs En by upos`. This would show which parts of speech are statistically more common in French than in English — e.g. French might show overuse of `DET` (more articles) while English might show more `PART` (infinitival *to*).

Like collocations, the keyness measure (log-likelihood, log-ratio, chi-squared, etc.) and output settings are controlled via command-line flags.

## Non-query functions overview

Between the frequency and the output functions, the following functions are supported for a query name M :

| function | syntax | description |
| ------ | ------ | ------ |
| size  | size [M] | count how many results there are in M |
| count  | count [M] by att+ | count how many results there are for each token or region attribute |
| group | group [M] by att+ | synonym for count |
| sort | sort [M] by att+ | sort the result on a token or region attribute |
| cat | cat [M] | produce a KWIC list of the results of M |
| freq | freq [M] by att | similar to count but gives instances per million (IPM) |
| raw | raw [M] | one line per match with corpus positions and token forms |
| tabulate | tabulate [M] att+ | produce a table with the given columns |
| coll | coll [M] by att | window-based collocations sorted by association measure |
| dcoll | dcoll [M] [rels] by att | dependency-based collocations, optionally filtered by deprel/direction |
| keyness | keyness [M] [vs N] by att | words overrepresented in M vs rest of corpus (or vs named query N), using log-likelihood G² |


## Interactive settings

In interactive mode, output settings can be changed at any time with `set` and inspected with `show settings`. The setting names are the same as the corresponding command-line flags (without the `--` prefix), so that behaviour is consistent between batch and interactive use. The most important settings are:

| setting | default | description |
| ------ | ------ | ------ |
| context | 5 | symmetric KWIC context width in tokens |
| left | 5 | left context / collocation window |
| right | 5 | right context / collocation window |
| window | 5 | set left and right at once |
| limit | 20 | maximum number of results to display (KWIC hits, collocates, keywords, etc.) |
| offset | 0 | skip the first N results |
| measures | logdice | association measures for coll/dcoll/keyness (comma-separated) |
| min-freq | 5 | minimum co-occurrence frequency for collocations |
| attrs | (all) | token attributes to show in output |

For example: `set context 10` widens the KWIC display, `set measures logdice, mi` changes the default collocation measures, and `show settings` prints all current values.

## Corpus management

Apart from corpus query options, pando-CQL also offers some options related to
corpus management:

| function | description |
| ------ | ------ |
| drop M | drop the named query (or drop all) |
| show named | show all active named queries and token names |
| show attributes | show all token attributes in the corpus |
| show regions | show all region structures in the corpus |
| show regions TYPE | list individual regions of TYPE with their attributes |
| show values ATTR | list unique values + counts for a positional or region attribute |
| show info | show corpus overview: name, size, structures, attributes |
| show settings | show current values of all interactive settings |

