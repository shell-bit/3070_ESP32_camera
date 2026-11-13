"""Run on the server console. Passwords are never command-line arguments."""
import argparse
import getpass
import os
import secrets
from pathlib import Path
from user_auth import Accounts

root=Path(__file__).resolve().parent
accounts=Accounts(Path(os.environ.get('CAMERA_DATA_DIR',str(root/'ai-data')))/'auth')
parser=argparse.ArgumentParser()
parser.add_argument('action',choices=['bootstrap','create','reset'])
parser.add_argument('--username',default='admin')
parser.add_argument('--role',choices=['admin','viewer'],default='admin')
args=parser.parse_args()
if args.action=='bootstrap':
    with accounts.db() as c:
        count=c.execute('SELECT count(*) FROM users').fetchone()[0]
    if count:
        print('Accounts already exist; no password changed.')
    else:
        password=secrets.token_urlsafe(24)
        accounts.create('admin',password,'admin')
        path=accounts.folder/'initial-admin.txt'
        fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'w') as f:
            f.write('Username: admin\nPassword: '+password+'\n\nSign in over HTTPS. Change this password under Account, then delete this file.\n')
        print('Initial account created. Credentials are in '+str(path)+' (server owner only).')
else:
    password=getpass.getpass('New password (12+ characters): ')
    if password!=getpass.getpass('Confirm new password: '):
        raise SystemExit('Passwords do not match.')
    if args.action=='create':
        accounts.create(args.username,password,args.role)
    else:
        with accounts.db() as c:
            row=c.execute('SELECT id FROM users WHERE username=?',(args.username.strip().lower(),)).fetchone()
        if not row: raise SystemExit('Account not found.')
        accounts.change_user(row['id'],None,True,password,None)
    print('Account saved.')
