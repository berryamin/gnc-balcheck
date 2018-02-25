# GNU Cash Balance Checker

## Description

[GnuCash](https://www.gnucash.org/) is a fantastic and powerful open-source
accounting package which allows one to easily track their finances.
GnuCash supports a MySQL backend which gives the user the option to store
the database of accounts and transactions on a central server (although
true multi-user concurrency is not officially supported).

This small utility connects to the specified database, looks up the given
account name, fetches and adds up all the transactions for that account
and then displays the result back to the user in the format they requested.

### Background
I have a small embedded headless Linux device with an LED display that
I wish to use to display the balance for one of my GNC accounts.
This utility was written as a fast and lightweight (not to mention unofficial)
way to extract the account's current balance out of the database.

### Pre-reqs
Other than a standard toolchain, you will need the MySQL client core:

`sudo apt-get install mysql-client-core-5.7`

### Compiling
There is only one source file currently. Build it using:

`gcc -Wall gnc_balcheck.c -o balcheck $(mysql_config --cflags --libs)`

### Using the program
The usage text in the source code is fairly explanatory.
Most people don't want to provide their MySQL password as an argument over the
command line so this utility has the means to read a "credentials" file which
must be in the format: `<username>:<password>`

The permissions of this file must be set to 0400 otherwise the program
will refuse to read it. A sample usage of the program might be:

`./balcheck -H 192.168.10.148 -c ~/.bcreds_file 'some_gnucash_account'`

### Future improvements

- Take the currency into account
- Improve efficiency


### Disclaimer
This program is in no way affiliated to GnuCash, nor is it endorsed by
them in any way. The developers of GnuCash may change their database
schema at will in the future and render this utility broken.
