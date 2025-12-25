lion@lion-E500:~/work/Nimbus$ sudo mysql -e "CREATE USER IF NOT EXISTS 'nimbus_user'@'127.0.0.1' IDENTIFIED BY 'Nimbus@123456';
GRANT SELECT,INSERT,UPDATE,DELETE ON nimbus.* TO 'nimbus_user'@'127.0.0.1';
FLUSH PRIVILEGES;"