create database cppwebserver;

USE cppwebserver;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
)ENGINE=InnoDB;

INSERT INTO user(username, passwd) VALUES('root', '123456');
