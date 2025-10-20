import 'package:smart_car_app/home.dart';
import 'package:smart_car_app/service/database.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';
import 'package:google_sign_in/google_sign_in.dart';

class AuthMethods {
  final FirebaseAuth auth = FirebaseAuth.instance;

  Future<User?> getCurrentUser() async {
    return auth.currentUser;
  }

  signInWithGoogle(BuildContext context) async {
    final FirebaseAuth firebaseAuth = FirebaseAuth.instance;
    final GoogleSignIn googleSignIn = GoogleSignIn();

    final GoogleSignInAccount? googleSignInAccount = await googleSignIn.signIn();

    // user cancelled the Google sign-in
    if (googleSignInAccount == null) return;

    final GoogleSignInAuthentication googleSignInAuthentication =
        await googleSignInAccount.authentication;

    final AuthCredential credential = GoogleAuthProvider.credential(
      idToken: googleSignInAuthentication.idToken,
      accessToken: googleSignInAuthentication.accessToken,
    );

    final UserCredential result = await firebaseAuth.signInWithCredential(credential);
    final User? userDetails = result.user;

    if (userDetails != null) {
      final Map<String, dynamic> userInfoMap = {
        "email": userDetails.email,
        "name": userDetails.displayName,
        "imgUrl": userDetails.photoURL,
        "id": userDetails.uid
      };
      await DatabaseMethods().addUser(userDetails.uid, userInfoMap);
      // ignore: use_build_context_synchronously
      Navigator.push(context, MaterialPageRoute(builder: (context) => Home()));
    }
  }
}