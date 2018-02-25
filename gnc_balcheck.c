#include <stdio.h>
#include <mysql.h>
#include <my_global.h>
#include <sys/stat.h>
#include <getopt.h>

#define PROG_VERSION     "0.2"

/* Default defines */
#define DEFAULT_MYSQL_DB      "gnucash"
#define DEFAULT_MYSQL_HOST    "localhost"


static const char *prog_name;
static int do_nothing(const char *fmt, ...) { return 0; }
static int (*verbose_printf)(const char *fmt, ...) = do_nothing;

enum output_mode {
  OUTPUT_MODE_DEFAULT = 0,
  OUTPUT_MODE_NONE,    /* print nothing, return code only */
  OUTPUT_MODE_RAW,     /* Raw value only */
  OUTPUT_MODE_SCRIPT,  /* KEY=VALUE */
  OUTPUT_MODE_LAST
};

const char *output_mode_strs[OUTPUT_MODE_LAST] = {
  "NORMAL",
  "NONE",
  "RAW",
  "SCRIPT",
};

struct program_options {
  char *db_host;    /* Database host FIXME: port? */
  char *db_user;    /* Database username (overrides credentials file) */
  char *db_passwd;  /* Database password (overrides credentials file) */
  char *db_dbase;   /* Database to use */
  char *accnt_name; /* Account name to lookup */
  char *outfile;    /* Outfile to write result */
  char *creds_file; /* Path to credentials file */
  enum output_mode omode;  /* Output mode */
};

/* Parse a credentials file in the form user:password
 * Strings are not allocated and should not be free'd. Returns 0 on
 * success, -1 on error
 */
static int
parse_credentials_file(const char *filepath,
                       char **username, char **passwd)
{
  static char buffer[1024];
  struct stat sb;
  ssize_t rd;
  FILE *infile;
  char *pw;

  if (!filepath)
    return -1;

  /* Stat the file first so we can check her permissions */
  if (stat(filepath, &sb) < 0) {
    fprintf(stderr, "Failed to stat() credentials file: %s\n",
            strerror(errno));
    return -1;
  }
  /* Check that it is a) not too big and b) that the permission are 0400 */
  if (!S_ISREG(sb.st_mode) || sb.st_size < 1 || sb.st_size >= 1024) {
    fprintf(stderr, "Invalid file type or size exceeds permitted limit\n");
    return -1;
  } else if ((sb.st_mode & ~(S_IRUSR | S_IFMT)) != 0) {
    fprintf(stderr, "Invalid mode on credentials file, must be 0400\n");
    return -1;
  }

  if ((infile = fopen(filepath, "r")) == NULL) {
    fprintf(stderr, "Failed to open credentials file: %s\n",
            strerror(errno));
    return -1;
  }
  /* Read the file. It shouldn't be bigger than the buffer */
  if ((rd = fread(buffer, sizeof(char), sizeof(buffer)-1, infile)) < 1) {
    fprintf(stderr, "Failed to read from credentials file: %s\n",
            rd < 1 ? "short read" : strerror(errno));
    goto out_fail;
  }
  buffer[rd-1] = '\0';
  if ((pw = strstr(buffer, ":")) == NULL) {
     fprintf(stderr, "Missing separator in credentials file\n");
     goto out_fail;
  }
  *pw = '\0';
  fclose(infile);
  if (username)
    *username = buffer;
  if (passwd)
    *passwd = ++pw;
  verbose_printf("Parsed credentials file '%s'\n", filepath);
  return 0;

out_fail:
  fclose(infile);
  return -1;
}

static char *
gnc_get_account_guid(MYSQL *con, const char *accnt_name)
{
  MYSQL_RES *result = NULL;
  MYSQL_ROW row;
  my_ulonglong row_cnt;
  char query_buf[1024];
  char *accnt_guid = NULL;

  snprintf(query_buf, sizeof(query_buf),
           "SELECT name,guid FROM accounts "
           "WHERE name = '%s'", accnt_name);

  /* Lookup the account name */
  if (mysql_query(con, query_buf) ||
      (result = mysql_store_result(con)) == NULL) {
    fprintf(stderr, "%s: Failed to lookup acccount '%s': %s\n",
            __func__, accnt_name, mysql_error(con));
    return NULL;
  }

  row_cnt = mysql_num_rows(result);
  if (row_cnt == 0) {
    fprintf(stderr, "%s: Unable to find account '%s' in database!\n",
            __func__, accnt_name);
    goto out_point;
  } else if (row_cnt > 1) {
    fprintf(stderr, "%s: Duplicate row data returned for '%s' (%llu rows)\n",
            __func__, accnt_name, row_cnt);
    goto out_point;
  }
  if ((row = mysql_fetch_row(result)) == NULL) {
    fprintf(stderr, "%s: Failed to fetch account info for '%s': %s\n",
            __func__, accnt_name, mysql_error(con));
    goto out_point;
  }

  verbose_printf("Account name: '%s'  GUID: '%s'\n",
                 row[0], row[1]);

  accnt_guid = strdup(row[1]);

out_point:
  mysql_free_result(result);
  return accnt_guid;
}

static int
gnc_get_account_balance(MYSQL *con, const char *accnt_name,
                        double *balance)
{
  MYSQL_RES *result = NULL;
  my_ulonglong row_cnt;
  char query_buf[1024];
  char *accnt_guid = NULL;
  double bal = 0.0;
  int ret = -1;
  int i;

  /* Get the account GUID first */
  if ((accnt_guid = gnc_get_account_guid(con, accnt_name)) == NULL) {
    return -1;
  }

  /* Get transactions */
  snprintf(query_buf, sizeof(query_buf),
           "SELECT value_num,value_denom,transactions.enter_date,transactions.description "
           "FROM splits,transactions WHERE splits.account_guid = '%s'"
           "AND splits.tx_guid = transactions.guid "
           "ORDER BY transactions.enter_date", accnt_guid);

  /* Get the transactions */
  if (mysql_query(con, query_buf) ||
      (result = mysql_store_result(con)) == NULL) {
    fprintf(stderr, "%s: Failed to get transactions for '%s': %s\n",
            __func__, accnt_name, mysql_error(con));
    goto out_point;
  }

  row_cnt = mysql_num_rows(result);
  verbose_printf ("Retrieved %llu transactions(s)\n", row_cnt);
  for (i = 0; i < (int) row_cnt; i++) {
    MYSQL_ROW r = mysql_fetch_row(result);
    double tmp;
    int num = 0;
    int denom = 0;

    if (r == NULL) {
      fprintf(stderr, "%s: Premature end to result data (%d/%llu): %s\n",
              __func__, i, row_cnt, mysql_error(con));
      goto out_point;
    }
    num = strtol(r[0], NULL, 10);
    denom = strtol(r[1], NULL, 10);
    if (denom <= 0) {
      fprintf(stderr, "%s: Invalid denom (%d) in result data row (%d/%llu)\n",
              __func__, denom, i, row_cnt);
      goto out_point;
    }
    tmp = (float) num / (float) denom;
    verbose_printf ("Transaction #%03d [%s] %4.2f kr (%s)\n",
                    i, r[2], tmp, r[3]);
    bal += tmp;
  }

  verbose_printf ("Balance of '%s' [%s]: %.2fkr\n",
                  accnt_name, accnt_guid, bal);
  *balance = bal;
  ret = 0;

out_point:
  if (result)
    mysql_free_result(result);
  free(accnt_guid);
  return ret;
}

static void
usage(int code, const char *msg)
{
  int i;
  fprintf(stderr, "GnuCash MySQL Account Balance Checker v%s\n",
          PROG_VERSION);
  if (msg) {
    fprintf(stderr, "\nError: %s\n", msg);
  }
  fprintf(stderr, "\nUsage: %s [options] <account_name>\n"
                  "\n Options:\n\n"
                  "    --host        -H    MySQL server (default: %s)\n"
                  "    --database    -d    Database to use (default: %s)\n"
                  "    --username    -u    MySQL username\n"
                  "    --password    -p    MySQL password\n"
                  "    --creds-file  -c    Optional credentials file\n"
                  "    --outfile     -o    File to write (default stdout)\n"
                  "    --outmode     -m    Output mode (default: %s)\n"
                  "    --verbose     -v    Verbose output\n"
                  "    --help        -h    This useful text\n\n",
                  prog_name, DEFAULT_MYSQL_HOST,
                  DEFAULT_MYSQL_DB,
                  output_mode_strs[OUTPUT_MODE_DEFAULT]);
  fprintf(stderr, "Supported output modes:\n");
  for (i = 0; i < OUTPUT_MODE_LAST; i++)
    fprintf(stderr, "\t%s\n", output_mode_strs[i]);

  fprintf(stderr, "\n");
  exit(code);
}

int main(int argc, char **argv)
{
  struct program_options opts = { .db_host = DEFAULT_MYSQL_HOST,
                                  .db_dbase = DEFAULT_MYSQL_DB,
                                };
  static struct option long_opts[] = {
    { "host",        required_argument, 0, 'H' },
    { "database",    required_argument, 0, 'd' },
    { "username",    required_argument, 0, 'u' },
    { "password",    required_argument, 0, 'p' },
    { "creds-file",  required_argument, 0, 'c' },
    { "outfile",     required_argument, 0, 'o' },
    { "outmode",     required_argument, 0, 'm' },
    { "verbose",     no_argument,       0, 'v' },
    { "help",        no_argument,       0, 'h' },
    { NULL, 0, 0, 0 },
  };

  FILE *outfile = NULL;
  time_t tv;
  MYSQL *con = NULL;
  double accnt_balance = 0.0;
  char *accnt_name;
  int option_index;
  int i;
  int c;

  prog_name = argv[0];
  while ((c = getopt_long(argc, argv, "H:d:u:p:c:o:m:vh",
                          long_opts, &option_index)) != -1) {
    switch (c) {
    case 'H':
      opts.db_host = optarg;
      break;
    case 'd':
      opts.db_dbase = optarg;
      break;
    case 'u':
      opts.db_user = optarg;
      break;
    case 'p':
      opts.db_passwd = optarg;
      break;
    case 'c':
      opts.creds_file = optarg;
      break;
    case 'm':
      for (i = 0, opts.omode = OUTPUT_MODE_LAST; i < OUTPUT_MODE_LAST; i++) {
        if (!strcasecmp(output_mode_strs[i], optarg)) {
          opts.omode = i;
          break;
        }
      }
      if (opts.omode >= OUTPUT_MODE_LAST)
          usage(EXIT_FAILURE, "Invalid output mode");
      break;
    case 'v':
      verbose_printf = printf;
      break;
    case 'o':
      opts.outfile = optarg;
      break;
    case 'h':
      usage(EXIT_SUCCESS, NULL);
      break;
    default:
      usage(EXIT_FAILURE, "Illegal option");
      break;
    }
  }

  if (optind >= argc) {
    usage(EXIT_FAILURE, "Missing account name argument");
  }
  accnt_name = argv[optind];

  /* Check if we have a credentials file. If the user hasn't specified
   * a username or password on the command line, it will be used from
   * the credentials file. Providing both a username and password will
   * result in the creds file not even being read.
   */
  if (opts.creds_file) {
    if (opts.db_user && opts.db_passwd) {
      verbose_printf("Command line options override credentials file!\n");
    } else if (parse_credentials_file(opts.creds_file,
                                      opts.db_user   ? NULL : &opts.db_user,
                                      opts.db_passwd ? NULL : &opts.db_passwd) != 0) {
      fprintf(stderr, "Unable to get credentials from file\n");
      exit(EXIT_FAILURE);
    }
  }
  /* Check we actually got a username and password */
  if (!opts.db_user || !opts.db_passwd) {
    char tbuf[200];

    snprintf(tbuf, sizeof(tbuf), "Missing %s%s for database",
             !opts.db_user   ? "[username]" : "",
             !opts.db_passwd ? "[password]" : "");
    usage(EXIT_FAILURE, tbuf);
  }

  /* Open the output file */
  if (opts.outfile) {
    if ((outfile = fopen(opts.outfile, "w")) == NULL) {
      fprintf(stderr, "Unable to open output file '%s': %s\n",
              opts.outfile, strerror(errno));
      goto out_fail;
    }
    verbose_printf("Opened file '%s' for output\n", opts.outfile);
  }

  /* Initiate the MySQL object */
  if ((con = mysql_init(NULL)) == NULL) {
    fprintf(stderr, "Unable to initialise MySQL: %s\n", mysql_error(con));
    goto out_fail;
  }

  /* Try to connect to the database */
  verbose_printf("Attempting to connect to MySQL server '%s', database '%s' "
                 "with user '%s'\n",
                 opts.db_host, opts.db_dbase, opts.db_user);
  if (mysql_real_connect(con, opts.db_host, opts.db_user, opts.db_passwd,
                         opts.db_dbase, 0, NULL, 0) == NULL) {
      fprintf(stderr, "Failed to connect to database: %s\n", mysql_error(con));
      goto out_fail;
  }

  verbose_printf("Connected to server '%s'. MySQL client version: %s\n",
                 opts.db_host, mysql_get_client_info());

  /* Obtain the account balance */
  if (gnc_get_account_balance(con, accnt_name, &accnt_balance) != 0) {
    /* Errors logged by callee function(s) */
    goto out_fail;
  }

  switch (opts.omode) {

  case OUTPUT_MODE_DEFAULT:
    tv = time(NULL);
    fprintf(outfile ? outfile : stdout,
            "[%.24s] Account balance for '%s' is %.2f\n",
            ctime(&tv), accnt_name, accnt_balance);
    break;
  case OUTPUT_MODE_RAW:
    fprintf(outfile ? outfile : stdout,
            "%.2f", accnt_balance);
    break;
  case OUTPUT_MODE_SCRIPT:
    fprintf(outfile ? outfile : stdout,
            "ACCOUNT_NAME=\"%s\"\n"
            "ACCOUNT_BALANCE=\"%.2f\"\n",
            accnt_name, accnt_balance);
    break;
  default:
    break;
  }
  mysql_close(con);
  if (outfile)
    fclose(outfile);
  return EXIT_SUCCESS;

out_fail:
  if (outfile)
    fclose(outfile);
  if (con)
    mysql_close(con);
  return EXIT_FAILURE;
}
